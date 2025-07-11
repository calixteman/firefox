/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::{BuiltDisplayList, DisplayListWithCache, ColorF, DynamicProperties, Epoch, FontRenderMode};
use api::{PipelineId, PropertyBinding, PropertyBindingId, PropertyValue, MixBlendMode, StackingContext};
use api::units::*;
use api::channel::Sender;
use malloc_size_of::{MallocSizeOf, MallocSizeOfOps};
use crate::render_api::MemoryReport;
use crate::composite::CompositorKind;
use crate::clip::{ClipStore, ClipTree};
use crate::spatial_tree::SpatialTree;
use crate::frame_builder::FrameBuilderConfig;
use crate::hit_test::{HitTester, HitTestingScene, HitTestingSceneStats};
use crate::internal_types::FastHashMap;
use crate::picture::SurfaceInfo;
use crate::picture_graph::PictureGraph;
use crate::prim_store::{PrimitiveStore, PrimitiveStoreStats, PictureIndex, PrimitiveInstance};
use crate::tile_cache::TileCacheConfig;
use std::sync::Arc;

/// Stores a map of the animated property bindings for the current display list. These
/// can be used to animate the transform and/or opacity of a display list without
/// re-submitting the display list itself.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct SceneProperties {
    transform_properties: FastHashMap<PropertyBindingId, LayoutTransform>,
    float_properties: FastHashMap<PropertyBindingId, f32>,
    color_properties: FastHashMap<PropertyBindingId, ColorF>,
    current_properties: DynamicProperties,
    pending_properties: Option<DynamicProperties>,
}

impl SceneProperties {
    pub fn new() -> Self {
        SceneProperties {
            transform_properties: FastHashMap::default(),
            float_properties: FastHashMap::default(),
            color_properties: FastHashMap::default(),
            current_properties: DynamicProperties::default(),
            pending_properties: None,
        }
    }

    /// Reset the pending properties without flush.
    pub fn reset_properties(&mut self) {
        self.pending_properties = None;
    }

    /// Add to the current property list for this display list.
    pub fn add_properties(&mut self, properties: DynamicProperties) {
        let mut pending_properties = self.pending_properties
            .take()
            .unwrap_or_default();

        pending_properties.extend(properties);

        self.pending_properties = Some(pending_properties);
    }

    /// Add to the current transform property list for this display list.
    pub fn add_transforms(&mut self, transforms: Vec<PropertyValue<LayoutTransform>>) {
        let mut pending_properties = self.pending_properties
            .take()
            .unwrap_or_default();

        pending_properties.transforms.extend(transforms);

        self.pending_properties = Some(pending_properties);
    }

    /// Flush any pending updates to the scene properties. Returns
    /// true if the properties have changed since the last flush
    /// was called. This code allows properties to be changed by
    /// multiple reset_properties, add_properties and add_transforms calls
    /// during a single transaction, and still correctly determine if any
    /// properties have changed. This can have significant power
    /// saving implications, allowing a frame build to be skipped
    /// if the properties haven't changed in many cases.
    pub fn flush_pending_updates(&mut self) -> bool {
        let mut properties_changed = false;

        if let Some(ref pending_properties) = self.pending_properties {
            if *pending_properties != self.current_properties {
                self.transform_properties.clear();
                self.float_properties.clear();
                self.color_properties.clear();

                for property in &pending_properties.transforms {
                    self.transform_properties
                        .insert(property.key.id, property.value);
                }

                for property in &pending_properties.floats {
                    self.float_properties
                        .insert(property.key.id, property.value);
                }

                for property in &pending_properties.colors {
                    self.color_properties
                        .insert(property.key.id, property.value);
                }

                self.current_properties = pending_properties.clone();
                properties_changed = true;
            }
        }

        properties_changed
    }

    /// Get the current value for a transform property.
    pub fn resolve_layout_transform(
        &self,
        property: &PropertyBinding<LayoutTransform>,
    ) -> LayoutTransform {
        match *property {
            PropertyBinding::Value(value) => value,
            PropertyBinding::Binding(ref key, v) => {
                self.transform_properties
                    .get(&key.id)
                    .cloned()
                    .unwrap_or(v)
            }
        }
    }

    /// Get the current value for a float property.
    pub fn resolve_float(
        &self,
        property: &PropertyBinding<f32>
    ) -> f32 {
        match *property {
            PropertyBinding::Value(value) => value,
            PropertyBinding::Binding(ref key, v) => {
                self.float_properties
                    .get(&key.id)
                    .cloned()
                    .unwrap_or(v)
            }
        }
    }

    pub fn float_properties(&self) -> &FastHashMap<PropertyBindingId, f32> {
        &self.float_properties
    }

    /// Get the current value for a color property.
    pub fn resolve_color(
        &self,
        property: &PropertyBinding<ColorF>
    ) -> ColorF {
        match *property {
            PropertyBinding::Value(value) => value,
            PropertyBinding::Binding(ref key, v) => {
                self.color_properties
                    .get(&key.id)
                    .cloned()
                    .unwrap_or(v)
            }
        }
    }

    pub fn color_properties(&self) -> &FastHashMap<PropertyBindingId, ColorF> {
        &self.color_properties
    }

}

/// A representation of the layout within the display port for a given document or iframe.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Clone)]
pub struct ScenePipeline {
    pub display_list: DisplayListWithCache,
}

/// A complete representation of the layout bundling visible pipelines together.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Clone)]
pub struct Scene {
    pub root_pipeline_id: Option<PipelineId>,
    pub pipelines: FastHashMap<PipelineId, ScenePipeline>,
    pub pipeline_epochs: FastHashMap<PipelineId, Epoch>,
}

impl Scene {
    pub fn new() -> Self {
        Scene {
            root_pipeline_id: None,
            pipelines: FastHashMap::default(),
            pipeline_epochs: FastHashMap::default(),
        }
    }

    pub fn set_root_pipeline_id(&mut self, pipeline_id: PipelineId) {
        self.root_pipeline_id = Some(pipeline_id);
    }

    pub fn set_display_list(
        &mut self,
        pipeline_id: PipelineId,
        epoch: Epoch,
        display_list: BuiltDisplayList,
    ) {
        // Adds a cache to the given display list. If this pipeline already had
        // a display list before, that display list is updated and used instead.
        let display_list = match self.pipelines.remove(&pipeline_id) {
            Some(mut pipeline) => {
                pipeline.display_list.update(display_list);
                pipeline.display_list
            }
            None => DisplayListWithCache::new_from_list(display_list)
        };

        let new_pipeline = ScenePipeline {
            display_list,
        };

        self.pipelines.insert(pipeline_id, new_pipeline);
        self.pipeline_epochs.insert(pipeline_id, epoch);
    }

    pub fn remove_pipeline(&mut self, pipeline_id: PipelineId) {
        if self.root_pipeline_id == Some(pipeline_id) {
            self.root_pipeline_id = None;
        }
        self.pipelines.remove(&pipeline_id);
        self.pipeline_epochs.remove(&pipeline_id);
    }

    pub fn update_epoch(&mut self, pipeline_id: PipelineId, epoch: Epoch) {
        self.pipeline_epochs.insert(pipeline_id, epoch);
    }

    pub fn has_root_pipeline(&self) -> bool {
        if let Some(ref root_id) = self.root_pipeline_id {
            return self.pipelines.contains_key(root_id);
        }

        false
    }

    pub fn report_memory(
        &self,
        ops: &mut MallocSizeOfOps,
        report: &mut MemoryReport
    ) {
        for (_, pipeline) in &self.pipelines {
            report.display_list += pipeline.display_list.size_of(ops)
        }
    }
}

pub trait StackingContextHelpers {
    fn mix_blend_mode_for_compositing(&self) -> Option<MixBlendMode>;
}

impl StackingContextHelpers for StackingContext {
    fn mix_blend_mode_for_compositing(&self) -> Option<MixBlendMode> {
        match self.mix_blend_mode {
            MixBlendMode::Normal => None,
            _ => Some(self.mix_blend_mode),
        }
    }
}


/// WebRender's internal representation of the scene.
pub struct BuiltScene {
    pub has_root_pipeline: bool,
    pub pipeline_epochs: FastHashMap<PipelineId, Epoch>,
    pub output_rect: DeviceIntRect,
    pub prim_store: PrimitiveStore,
    pub clip_store: ClipStore,
    pub config: FrameBuilderConfig,
    pub hit_testing_scene: Arc<HitTestingScene>,
    pub tile_cache_config: TileCacheConfig,
    pub snapshot_pictures: Vec<PictureIndex>,
    pub tile_cache_pictures: Vec<PictureIndex>,
    pub picture_graph: PictureGraph,
    pub num_plane_splitters: usize,
    pub prim_instances: Vec<PrimitiveInstance>,
    pub surfaces: Vec<SurfaceInfo>,
    pub clip_tree: ClipTree,

    /// Deallocating memory outside of the thread that allocated it causes lock
    /// contention in jemalloc. To avoid this we send the built scene back to
    /// the scene builder thread when we don't need it anymore, and in the process,
    /// also reuse some allocations.
    pub recycler_tx: Option<Sender<BuiltScene>>,
}

impl BuiltScene {
    pub fn empty() -> Self {
        BuiltScene {
            has_root_pipeline: false,
            pipeline_epochs: FastHashMap::default(),
            output_rect: DeviceIntRect::zero(),
            prim_store: PrimitiveStore::new(&PrimitiveStoreStats::empty()),
            clip_store: ClipStore::new(),
            hit_testing_scene: Arc::new(HitTestingScene::new(&HitTestingSceneStats::empty())),
            tile_cache_config: TileCacheConfig::new(0),
            snapshot_pictures: Vec::new(),
            tile_cache_pictures: Vec::new(),
            picture_graph: PictureGraph::new(),
            num_plane_splitters: 0,
            prim_instances: Vec::new(),
            surfaces: Vec::new(),
            clip_tree: ClipTree::new(),
            recycler_tx: None,
            config: FrameBuilderConfig {
                default_font_render_mode: FontRenderMode::Mono,
                dual_source_blending_is_supported: false,
                testing: false,
                gpu_supports_fast_clears: false,
                gpu_supports_advanced_blend: false,
                advanced_blend_is_coherent: false,
                gpu_supports_render_target_partial_update: true,
                external_images_require_copy: false,
                batch_lookback_count: 0,
                background_color: None,
                compositor_kind: CompositorKind::default(),
                tile_size_override: None,
                max_surface_override: None,
                max_depth_ids: 0,
                max_target_size: 0,
                force_invalidation: false,
                is_software: false,
                low_quality_pinch_zoom: false,
                max_shared_surface_size: 2048,
                enable_dithering: false,
            },
        }
    }

    /// Send the scene back to the scene builder thread so that recycling/deallocations
    /// can happen there.
    pub fn recycle(mut self) {
        if let Some(tx) = self.recycler_tx.take() {
            let _ = tx.send(self);
        }
    }

    /// Get the memory usage statistics to pre-allocate for the next scene.
    pub fn get_stats(&self) -> SceneStats {
        SceneStats {
            prim_store_stats: self.prim_store.get_stats(),
            hit_test_stats: self.hit_testing_scene.get_stats(),
        }
    }

    pub fn create_hit_tester(
        &mut self,
        spatial_tree: &SpatialTree,
    ) -> HitTester {
        HitTester::new(
            Arc::clone(&self.hit_testing_scene),
            spatial_tree,
        )
    }
}

/// Stores the allocation sizes of various arrays in the built
/// scene. This is retrieved from the current frame builder
/// and used to reserve an approximately correct capacity of
/// the arrays for the next scene that is getting built.
pub struct SceneStats {
    pub prim_store_stats: PrimitiveStoreStats,
    pub hit_test_stats: HitTestingSceneStats,
}

impl SceneStats {
    pub fn empty() -> Self {
        SceneStats {
            prim_store_stats: PrimitiveStoreStats::empty(),
            hit_test_stats: HitTestingSceneStats::empty(),
        }
    }
}
