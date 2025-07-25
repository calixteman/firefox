/*!
Frontend for [SPIR-V][spv] (Standard Portable Intermediate Representation).

## ID lookups

Our IR links to everything with `Handle`, while SPIR-V uses IDs.
In order to keep track of the associations, the parser has many lookup tables.
There map `spv::Word` into a specific IR handle, plus potentially a bit of
extra info, such as the related SPIR-V type ID.
TODO: would be nice to find ways that avoid looking up as much

## Inputs/Outputs

We create a private variable for each input/output. The relevant inputs are
populated at the start of an entry point. The outputs are saved at the end.

The function associated with an entry point is wrapped in another function,
such that we can handle any `Return` statements without problems.

## Row-major matrices

We don't handle them natively, since the IR only expects column majority.
Instead, we detect when such matrix is accessed in the `OpAccessChain`,
and we generate a parallel expression that loads the value, but transposed.
This value then gets used instead of `OpLoad` result later on.

[spv]: https://www.khronos.org/registry/SPIR-V/
*/

mod convert;
mod error;
mod function;
mod image;
mod null;

pub use error::Error;

use alloc::{borrow::ToOwned, format, string::String, vec, vec::Vec};
use core::{convert::TryInto, mem, num::NonZeroU32};

use half::f16;
use petgraph::graphmap::GraphMap;

use super::atomic_upgrade::Upgrades;
use crate::{
    arena::{Arena, Handle, UniqueArena},
    path_like::PathLikeOwned,
    proc::{Alignment, Layouter},
    FastHashMap, FastHashSet, FastIndexMap,
};
use convert::*;
use function::*;

pub const SUPPORTED_CAPABILITIES: &[spirv::Capability] = &[
    spirv::Capability::Shader,
    spirv::Capability::VulkanMemoryModel,
    spirv::Capability::ClipDistance,
    spirv::Capability::CullDistance,
    spirv::Capability::SampleRateShading,
    spirv::Capability::DerivativeControl,
    spirv::Capability::Matrix,
    spirv::Capability::ImageQuery,
    spirv::Capability::Sampled1D,
    spirv::Capability::Image1D,
    spirv::Capability::SampledCubeArray,
    spirv::Capability::ImageCubeArray,
    spirv::Capability::StorageImageExtendedFormats,
    spirv::Capability::Int8,
    spirv::Capability::Int16,
    spirv::Capability::Int64,
    spirv::Capability::Int64Atomics,
    spirv::Capability::Float16,
    spirv::Capability::AtomicFloat32AddEXT,
    spirv::Capability::Float64,
    spirv::Capability::Geometry,
    spirv::Capability::MultiView,
    spirv::Capability::StorageBuffer16BitAccess,
    spirv::Capability::UniformAndStorageBuffer16BitAccess,
    spirv::Capability::GroupNonUniform,
    spirv::Capability::GroupNonUniformVote,
    spirv::Capability::GroupNonUniformArithmetic,
    spirv::Capability::GroupNonUniformBallot,
    spirv::Capability::GroupNonUniformShuffle,
    spirv::Capability::GroupNonUniformShuffleRelative,
    // tricky ones
    spirv::Capability::UniformBufferArrayDynamicIndexing,
    spirv::Capability::StorageBufferArrayDynamicIndexing,
];
pub const SUPPORTED_EXTENSIONS: &[&str] = &[
    "SPV_KHR_storage_buffer_storage_class",
    "SPV_KHR_vulkan_memory_model",
    "SPV_KHR_multiview",
    "SPV_EXT_shader_atomic_float_add",
    "SPV_KHR_16bit_storage",
];
pub const SUPPORTED_EXT_SETS: &[&str] = &["GLSL.std.450"];

#[derive(Copy, Clone)]
pub struct Instruction {
    op: spirv::Op,
    wc: u16,
}

impl Instruction {
    const fn expect(self, count: u16) -> Result<(), Error> {
        if self.wc == count {
            Ok(())
        } else {
            Err(Error::InvalidOperandCount(self.op, self.wc))
        }
    }

    fn expect_at_least(self, count: u16) -> Result<u16, Error> {
        self.wc
            .checked_sub(count)
            .ok_or(Error::InvalidOperandCount(self.op, self.wc))
    }
}

impl crate::TypeInner {
    fn can_comparison_sample(&self, module: &crate::Module) -> bool {
        match *self {
            crate::TypeInner::Image {
                class:
                    crate::ImageClass::Sampled {
                        kind: crate::ScalarKind::Float,
                        multi: false,
                    },
                ..
            } => true,
            crate::TypeInner::Sampler { .. } => true,
            crate::TypeInner::BindingArray { base, .. } => {
                module.types[base].inner.can_comparison_sample(module)
            }
            _ => false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, PartialOrd)]
pub enum ModuleState {
    Empty,
    Capability,
    Extension,
    ExtInstImport,
    MemoryModel,
    EntryPoint,
    ExecutionMode,
    Source,
    Name,
    ModuleProcessed,
    Annotation,
    Type,
    Function,
}

trait LookupHelper {
    type Target;
    fn lookup(&self, key: spirv::Word) -> Result<&Self::Target, Error>;
}

impl<T> LookupHelper for FastHashMap<spirv::Word, T> {
    type Target = T;
    fn lookup(&self, key: spirv::Word) -> Result<&T, Error> {
        self.get(&key).ok_or(Error::InvalidId(key))
    }
}

impl crate::ImageDimension {
    const fn required_coordinate_size(&self) -> Option<crate::VectorSize> {
        match *self {
            crate::ImageDimension::D1 => None,
            crate::ImageDimension::D2 => Some(crate::VectorSize::Bi),
            crate::ImageDimension::D3 => Some(crate::VectorSize::Tri),
            crate::ImageDimension::Cube => Some(crate::VectorSize::Tri),
        }
    }
}

type MemberIndex = u32;

bitflags::bitflags! {
    #[derive(Clone, Copy, Debug, Default)]
    struct DecorationFlags: u32 {
        const NON_READABLE = 0x1;
        const NON_WRITABLE = 0x2;
    }
}

impl DecorationFlags {
    fn to_storage_access(self) -> crate::StorageAccess {
        let mut access = crate::StorageAccess::LOAD | crate::StorageAccess::STORE;
        if self.contains(DecorationFlags::NON_READABLE) {
            access &= !crate::StorageAccess::LOAD;
        }
        if self.contains(DecorationFlags::NON_WRITABLE) {
            access &= !crate::StorageAccess::STORE;
        }
        access
    }
}

#[derive(Debug, PartialEq)]
enum Majority {
    Column,
    Row,
}

#[derive(Debug, Default)]
struct Decoration {
    name: Option<String>,
    built_in: Option<spirv::Word>,
    location: Option<spirv::Word>,
    desc_set: Option<spirv::Word>,
    desc_index: Option<spirv::Word>,
    specialization_constant_id: Option<spirv::Word>,
    storage_buffer: bool,
    offset: Option<spirv::Word>,
    array_stride: Option<NonZeroU32>,
    matrix_stride: Option<NonZeroU32>,
    matrix_major: Option<Majority>,
    invariant: bool,
    interpolation: Option<crate::Interpolation>,
    sampling: Option<crate::Sampling>,
    flags: DecorationFlags,
}

impl Decoration {
    fn debug_name(&self) -> &str {
        match self.name {
            Some(ref name) => name.as_str(),
            None => "?",
        }
    }

    const fn resource_binding(&self) -> Option<crate::ResourceBinding> {
        match *self {
            Decoration {
                desc_set: Some(group),
                desc_index: Some(binding),
                ..
            } => Some(crate::ResourceBinding { group, binding }),
            _ => None,
        }
    }

    fn io_binding(&self) -> Result<crate::Binding, Error> {
        match *self {
            Decoration {
                built_in: Some(built_in),
                location: None,
                invariant,
                ..
            } => Ok(crate::Binding::BuiltIn(map_builtin(built_in, invariant)?)),
            Decoration {
                built_in: None,
                location: Some(location),
                interpolation,
                sampling,
                ..
            } => Ok(crate::Binding::Location {
                location,
                interpolation,
                sampling,
                blend_src: None,
            }),
            _ => Err(Error::MissingDecoration(spirv::Decoration::Location)),
        }
    }
}

#[derive(Debug)]
struct LookupFunctionType {
    parameter_type_ids: Vec<spirv::Word>,
    return_type_id: spirv::Word,
}

struct LookupFunction {
    handle: Handle<crate::Function>,
    parameters_sampling: Vec<image::SamplingFlags>,
}

#[derive(Debug)]
struct EntryPoint {
    stage: crate::ShaderStage,
    name: String,
    early_depth_test: Option<crate::EarlyDepthTest>,
    workgroup_size: [u32; 3],
    variable_ids: Vec<spirv::Word>,
}

#[derive(Clone, Debug)]
struct LookupType {
    handle: Handle<crate::Type>,
    base_id: Option<spirv::Word>,
}

#[derive(Debug)]
enum Constant {
    Constant(Handle<crate::Constant>),
    Override(Handle<crate::Override>),
}

impl Constant {
    const fn to_expr(&self) -> crate::Expression {
        match *self {
            Self::Constant(c) => crate::Expression::Constant(c),
            Self::Override(o) => crate::Expression::Override(o),
        }
    }
}

#[derive(Debug)]
struct LookupConstant {
    inner: Constant,
    type_id: spirv::Word,
}

#[derive(Debug)]
enum Variable {
    Global,
    Input(crate::FunctionArgument),
    Output(crate::FunctionResult),
}

#[derive(Debug)]
struct LookupVariable {
    inner: Variable,
    handle: Handle<crate::GlobalVariable>,
    type_id: spirv::Word,
}

/// Information about SPIR-V result ids, stored in `Frontend::lookup_expression`.
#[derive(Clone, Debug)]
struct LookupExpression {
    /// The `Expression` constructed for this result.
    ///
    /// Note that, while a SPIR-V result id can be used in any block dominated
    /// by its definition, a Naga `Expression` is only in scope for the rest of
    /// its subtree. `Frontend::get_expr_handle` takes care of spilling the result
    /// to a `LocalVariable` which can then be used anywhere.
    handle: Handle<crate::Expression>,

    /// The SPIR-V type of this result.
    type_id: spirv::Word,

    /// The label id of the block that defines this expression.
    ///
    /// This is zero for globals, constants, and function parameters, since they
    /// originate outside any function's block.
    block_id: spirv::Word,
}

#[derive(Debug)]
struct LookupMember {
    type_id: spirv::Word,
    // This is true for either matrices, or arrays of matrices (yikes).
    row_major: bool,
}

#[derive(Clone, Debug)]
enum LookupLoadOverride {
    /// For arrays of matrices, we track them but not loading yet.
    Pending,
    /// For matrices, vectors, and scalars, we pre-load the data.
    Loaded(Handle<crate::Expression>),
}

#[derive(PartialEq)]
enum ExtendedClass {
    Global(crate::AddressSpace),
    Input,
    Output,
}

#[derive(Clone, Debug)]
pub struct Options {
    /// The IR coordinate space matches all the APIs except SPIR-V,
    /// so by default we flip the Y coordinate of the `BuiltIn::Position`.
    /// This flag can be used to avoid this.
    pub adjust_coordinate_space: bool,
    /// Only allow shaders with the known set of capabilities.
    pub strict_capabilities: bool,
    pub block_ctx_dump_prefix: Option<PathLikeOwned>,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            adjust_coordinate_space: true,
            strict_capabilities: true,
            block_ctx_dump_prefix: None,
        }
    }
}

/// An index into the `BlockContext::bodies` table.
type BodyIndex = usize;

/// An intermediate representation of a Naga [`Statement`].
///
/// `Body` and `BodyFragment` values form a tree: the `BodyIndex` fields of the
/// variants are indices of the child `Body` values in [`BlockContext::bodies`].
/// The `lower` function assembles the final `Statement` tree from this `Body`
/// tree. See [`BlockContext`] for details.
///
/// [`Statement`]: crate::Statement
#[derive(Debug)]
enum BodyFragment {
    BlockId(spirv::Word),
    If {
        condition: Handle<crate::Expression>,
        accept: BodyIndex,
        reject: BodyIndex,
    },
    Loop {
        /// The body of the loop. Its [`Body::parent`] is the block containing
        /// this `Loop` fragment.
        body: BodyIndex,

        /// The loop's continuing block. This is a grandchild: its
        /// [`Body::parent`] is the loop body block, whose index is above.
        continuing: BodyIndex,

        /// If the SPIR-V loop's back-edge branch is conditional, this is the
        /// expression that must be `false` for the back-edge to be taken, with
        /// `true` being for the "loop merge" (which breaks out of the loop).
        break_if: Option<Handle<crate::Expression>>,
    },
    Switch {
        selector: Handle<crate::Expression>,
        cases: Vec<(i32, BodyIndex)>,
        default: BodyIndex,
    },
    Break,
    Continue,
}

/// An intermediate representation of a Naga [`Block`].
///
/// This will be assembled into a `Block` once we've added spills for phi nodes
/// and out-of-scope expressions. See [`BlockContext`] for details.
///
/// [`Block`]: crate::Block
#[derive(Debug)]
struct Body {
    /// The index of the direct parent of this body
    parent: usize,
    data: Vec<BodyFragment>,
}

impl Body {
    /// Creates a new empty `Body` with the specified `parent`
    pub const fn with_parent(parent: usize) -> Self {
        Body {
            parent,
            data: Vec::new(),
        }
    }
}

#[derive(Debug)]
struct PhiExpression {
    /// The local variable used for the phi node
    local: Handle<crate::LocalVariable>,
    /// List of (expression, block)
    expressions: Vec<(spirv::Word, spirv::Word)>,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum MergeBlockInformation {
    LoopMerge,
    LoopContinue,
    SelectionMerge,
    SwitchMerge,
}

/// Fragments of Naga IR, to be assembled into `Statements` once data flow is
/// resolved.
///
/// We can't build a Naga `Statement` tree directly from SPIR-V blocks for three
/// main reasons:
///
/// - We parse a function's SPIR-V blocks in the order they appear in the file.
///   Within a function, SPIR-V requires that a block must precede any blocks it
///   structurally dominates, but doesn't say much else about the order in which
///   they must appear. So while we know we'll see control flow header blocks
///   before their child constructs and merge blocks, those children and the
///   merge blocks may appear in any order - perhaps even intermingled with
///   children of other constructs.
///
/// - A SPIR-V expression can be used in any SPIR-V block dominated by its
///   definition, whereas Naga expressions are scoped to the rest of their
///   subtree. This means that discovering an expression use later in the
///   function retroactively requires us to have spilled that expression into a
///   local variable back before we left its scope. (The docs for
///   [`Frontend::get_expr_handle`] explain this in more detail.)
///
/// - We translate SPIR-V OpPhi expressions as Naga local variables in which we
///   store the appropriate value before jumping to the OpPhi's block.
///
/// All these cases require us to go back and amend previously generated Naga IR
/// based on things we discover later. But modifying old blocks in arbitrary
/// spots in a `Statement` tree is awkward.
///
/// Instead, as we iterate through the function's body, we accumulate
/// control-flow-free fragments of Naga IR in the [`blocks`] table, while
/// building a skeleton of the Naga `Statement` tree in [`bodies`]. We note any
/// spills and temporaries we must introduce in [`phis`].
///
/// Finally, once we've processed the entire function, we add temporaries and
/// spills to the fragmentary `Blocks` as directed by `phis`, and assemble them
/// into the final Naga `Statement` tree as directed by `bodies`.
///
/// [`blocks`]: BlockContext::blocks
/// [`bodies`]: BlockContext::bodies
/// [`phis`]: BlockContext::phis
/// [`lower`]: function::lower
#[derive(Debug)]
struct BlockContext<'function> {
    /// Phi nodes encountered when parsing the function, used to generate spills
    /// to local variables.
    phis: Vec<PhiExpression>,

    /// Fragments of control-flow-free Naga IR.
    ///
    /// These will be stitched together into a proper [`Statement`] tree according
    /// to `bodies`, once parsing is complete.
    ///
    /// [`Statement`]: crate::Statement
    blocks: FastHashMap<spirv::Word, crate::Block>,

    /// Map from each SPIR-V block's label id to the index of the [`Body`] in
    /// [`bodies`] the block should append its contents to.
    ///
    /// Since each statement in a Naga [`Block`] dominates the next, we are sure
    /// to encounter their SPIR-V blocks in order. Thus, by having this table
    /// map a SPIR-V structured control flow construct's merge block to the same
    /// body index as its header block, when we encounter the merge block, we
    /// will simply pick up building the [`Body`] where the header left off.
    ///
    /// A function's first block is special: it is the only block we encounter
    /// without having seen its label mentioned in advance. (It's simply the
    /// first `OpLabel` after the `OpFunction`.) We thus assume that any block
    /// missing an entry here must be the first block, which always has body
    /// index zero.
    ///
    /// [`bodies`]: BlockContext::bodies
    /// [`Block`]: crate::Block
    body_for_label: FastHashMap<spirv::Word, BodyIndex>,

    /// SPIR-V metadata about merge/continue blocks.
    mergers: FastHashMap<spirv::Word, MergeBlockInformation>,

    /// A table of `Body` values, each representing a block in the final IR.
    ///
    /// The first element is always the function's top-level block.
    bodies: Vec<Body>,

    /// The module we're building.
    module: &'function mut crate::Module,

    /// Id of the function currently being processed
    function_id: spirv::Word,
    /// Expression arena of the function currently being processed
    expressions: &'function mut Arena<crate::Expression>,
    /// Local variables arena of the function currently being processed
    local_arena: &'function mut Arena<crate::LocalVariable>,
    /// Arguments of the function currently being processed
    arguments: &'function [crate::FunctionArgument],
    /// Metadata about the usage of function parameters as sampling objects
    parameter_sampling: &'function mut [image::SamplingFlags],
}

enum SignAnchor {
    Result,
    Operand,
}

pub struct Frontend<I> {
    data: I,
    data_offset: usize,
    state: ModuleState,
    layouter: Layouter,
    temp_bytes: Vec<u8>,
    ext_glsl_id: Option<spirv::Word>,
    future_decor: FastHashMap<spirv::Word, Decoration>,
    future_member_decor: FastHashMap<(spirv::Word, MemberIndex), Decoration>,
    lookup_member: FastHashMap<(Handle<crate::Type>, MemberIndex), LookupMember>,
    handle_sampling: FastHashMap<Handle<crate::GlobalVariable>, image::SamplingFlags>,

    /// A record of what is accessed by [`Atomic`] statements we've
    /// generated, so we can upgrade the types of their operands.
    ///
    /// [`Atomic`]: crate::Statement::Atomic
    upgrade_atomics: Upgrades,

    lookup_type: FastHashMap<spirv::Word, LookupType>,
    lookup_void_type: Option<spirv::Word>,
    lookup_storage_buffer_types: FastHashMap<Handle<crate::Type>, crate::StorageAccess>,
    lookup_constant: FastHashMap<spirv::Word, LookupConstant>,
    lookup_variable: FastHashMap<spirv::Word, LookupVariable>,
    lookup_expression: FastHashMap<spirv::Word, LookupExpression>,
    // Load overrides are used to work around row-major matrices
    lookup_load_override: FastHashMap<spirv::Word, LookupLoadOverride>,
    lookup_sampled_image: FastHashMap<spirv::Word, image::LookupSampledImage>,
    lookup_function_type: FastHashMap<spirv::Word, LookupFunctionType>,
    lookup_function: FastHashMap<spirv::Word, LookupFunction>,
    lookup_entry_point: FastHashMap<spirv::Word, EntryPoint>,
    // When parsing functions, each entry point function gets an entry here so that additional
    // processing for them can be performed after all function parsing.
    deferred_entry_points: Vec<(EntryPoint, spirv::Word)>,
    //Note: each `OpFunctionCall` gets a single entry here, indexed by the
    // dummy `Handle<crate::Function>` of the call site.
    deferred_function_calls: Vec<spirv::Word>,
    dummy_functions: Arena<crate::Function>,
    // Graph of all function calls through the module.
    // It's used to sort the functions (as nodes) topologically,
    // so that in the IR any called function is already known.
    function_call_graph: GraphMap<
        spirv::Word,
        (),
        petgraph::Directed,
        core::hash::BuildHasherDefault<rustc_hash::FxHasher>,
    >,
    options: Options,

    /// Maps for a switch from a case target to the respective body and associated literals that
    /// use that target block id.
    ///
    /// Used to preserve allocations between instruction parsing.
    switch_cases: FastIndexMap<spirv::Word, (BodyIndex, Vec<i32>)>,

    /// Tracks access to gl_PerVertex's builtins, it is used to cull unused builtins since initializing those can
    /// affect performance and the mere presence of some of these builtins might cause backends to error since they
    /// might be unsupported.
    ///
    /// The problematic builtins are: PointSize, ClipDistance and CullDistance.
    ///
    /// glslang declares those by default even though they are never written to
    /// (see <https://github.com/KhronosGroup/glslang/issues/1868>)
    gl_per_vertex_builtin_access: FastHashSet<crate::BuiltIn>,
}

impl<I: Iterator<Item = u32>> Frontend<I> {
    pub fn new(data: I, options: &Options) -> Self {
        Frontend {
            data,
            data_offset: 0,
            state: ModuleState::Empty,
            layouter: Layouter::default(),
            temp_bytes: Vec::new(),
            ext_glsl_id: None,
            future_decor: FastHashMap::default(),
            future_member_decor: FastHashMap::default(),
            handle_sampling: FastHashMap::default(),
            lookup_member: FastHashMap::default(),
            upgrade_atomics: Default::default(),
            lookup_type: FastHashMap::default(),
            lookup_void_type: None,
            lookup_storage_buffer_types: FastHashMap::default(),
            lookup_constant: FastHashMap::default(),
            lookup_variable: FastHashMap::default(),
            lookup_expression: FastHashMap::default(),
            lookup_load_override: FastHashMap::default(),
            lookup_sampled_image: FastHashMap::default(),
            lookup_function_type: FastHashMap::default(),
            lookup_function: FastHashMap::default(),
            lookup_entry_point: FastHashMap::default(),
            deferred_entry_points: Vec::default(),
            deferred_function_calls: Vec::default(),
            dummy_functions: Arena::new(),
            function_call_graph: GraphMap::new(),
            options: options.clone(),
            switch_cases: FastIndexMap::default(),
            gl_per_vertex_builtin_access: FastHashSet::default(),
        }
    }

    fn span_from(&self, from: usize) -> crate::Span {
        crate::Span::from(from..self.data_offset)
    }

    fn span_from_with_op(&self, from: usize) -> crate::Span {
        crate::Span::from((from - 4)..self.data_offset)
    }

    fn next(&mut self) -> Result<u32, Error> {
        if let Some(res) = self.data.next() {
            self.data_offset += 4;
            Ok(res)
        } else {
            Err(Error::IncompleteData)
        }
    }

    fn next_inst(&mut self) -> Result<Instruction, Error> {
        let word = self.next()?;
        let (wc, opcode) = ((word >> 16) as u16, (word & 0xffff) as u16);
        if wc == 0 {
            return Err(Error::InvalidWordCount);
        }
        let op = spirv::Op::from_u32(opcode as u32).ok_or(Error::UnknownInstruction(opcode))?;

        Ok(Instruction { op, wc })
    }

    fn next_string(&mut self, mut count: u16) -> Result<(String, u16), Error> {
        self.temp_bytes.clear();
        loop {
            if count == 0 {
                return Err(Error::BadString);
            }
            count -= 1;
            let chars = self.next()?.to_le_bytes();
            let pos = chars.iter().position(|&c| c == 0).unwrap_or(4);
            self.temp_bytes.extend_from_slice(&chars[..pos]);
            if pos < 4 {
                break;
            }
        }
        core::str::from_utf8(&self.temp_bytes)
            .map(|s| (s.to_owned(), count))
            .map_err(|_| Error::BadString)
    }

    fn next_decoration(
        &mut self,
        inst: Instruction,
        base_words: u16,
        dec: &mut Decoration,
    ) -> Result<(), Error> {
        let raw = self.next()?;
        let dec_typed = spirv::Decoration::from_u32(raw).ok_or(Error::InvalidDecoration(raw))?;
        log::trace!("\t\t{}: {:?}", dec.debug_name(), dec_typed);
        match dec_typed {
            spirv::Decoration::BuiltIn => {
                inst.expect(base_words + 2)?;
                dec.built_in = Some(self.next()?);
            }
            spirv::Decoration::Location => {
                inst.expect(base_words + 2)?;
                dec.location = Some(self.next()?);
            }
            spirv::Decoration::DescriptorSet => {
                inst.expect(base_words + 2)?;
                dec.desc_set = Some(self.next()?);
            }
            spirv::Decoration::Binding => {
                inst.expect(base_words + 2)?;
                dec.desc_index = Some(self.next()?);
            }
            spirv::Decoration::BufferBlock => {
                dec.storage_buffer = true;
            }
            spirv::Decoration::Offset => {
                inst.expect(base_words + 2)?;
                dec.offset = Some(self.next()?);
            }
            spirv::Decoration::ArrayStride => {
                inst.expect(base_words + 2)?;
                dec.array_stride = NonZeroU32::new(self.next()?);
            }
            spirv::Decoration::MatrixStride => {
                inst.expect(base_words + 2)?;
                dec.matrix_stride = NonZeroU32::new(self.next()?);
            }
            spirv::Decoration::Invariant => {
                dec.invariant = true;
            }
            spirv::Decoration::NoPerspective => {
                dec.interpolation = Some(crate::Interpolation::Linear);
            }
            spirv::Decoration::Flat => {
                dec.interpolation = Some(crate::Interpolation::Flat);
            }
            spirv::Decoration::Centroid => {
                dec.sampling = Some(crate::Sampling::Centroid);
            }
            spirv::Decoration::Sample => {
                dec.sampling = Some(crate::Sampling::Sample);
            }
            spirv::Decoration::NonReadable => {
                dec.flags |= DecorationFlags::NON_READABLE;
            }
            spirv::Decoration::NonWritable => {
                dec.flags |= DecorationFlags::NON_WRITABLE;
            }
            spirv::Decoration::ColMajor => {
                dec.matrix_major = Some(Majority::Column);
            }
            spirv::Decoration::RowMajor => {
                dec.matrix_major = Some(Majority::Row);
            }
            spirv::Decoration::SpecId => {
                dec.specialization_constant_id = Some(self.next()?);
            }
            other => {
                log::warn!("Unknown decoration {:?}", other);
                for _ in base_words + 1..inst.wc {
                    let _var = self.next()?;
                }
            }
        }
        Ok(())
    }

    /// Return the Naga [`Expression`] to use in `body_idx` to refer to the SPIR-V result `id`.
    ///
    /// Ideally, we would just have a map from each SPIR-V instruction id to the
    /// [`Handle`] for the Naga [`Expression`] we generated for it.
    /// Unfortunately, SPIR-V and Naga IR are different enough that such a
    /// straightforward relationship isn't possible.
    ///
    /// In SPIR-V, an instruction's result id can be used by any instruction
    /// dominated by that instruction. In Naga, an [`Expression`] is only in
    /// scope for the remainder of its [`Block`]. In pseudocode:
    ///
    /// ```ignore
    ///     loop {
    ///         a = f();
    ///         g(a);
    ///         break;
    ///     }
    ///     h(a);
    /// ```
    ///
    /// Suppose the calls to `f`, `g`, and `h` are SPIR-V instructions. In
    /// SPIR-V, both the `g` and `h` instructions are allowed to refer to `a`,
    /// because the loop body, including `f`, dominates both of them.
    ///
    /// But if `a` is a Naga [`Expression`], its scope ends at the end of the
    /// block it's evaluated in: the loop body. Thus, while the [`Expression`]
    /// we generate for `g` can refer to `a`, the one we generate for `h`
    /// cannot.
    ///
    /// Instead, the SPIR-V front end must generate Naga IR like this:
    ///
    /// ```ignore
    ///     var temp; // INTRODUCED
    ///     loop {
    ///         a = f();
    ///         g(a);
    ///         temp = a; // INTRODUCED
    ///     }
    ///     h(temp); // ADJUSTED
    /// ```
    ///
    /// In other words, where `a` is in scope, [`Expression`]s can refer to it
    /// directly; but once it is out of scope, we need to spill it to a
    /// temporary and refer to that instead.
    ///
    /// Given a SPIR-V expression `id` and the index `body_idx` of the [body]
    /// that wants to refer to it:
    ///
    /// - If the Naga [`Expression`] we generated for `id` is in scope in
    ///   `body_idx`, then we simply return its `Handle<Expression>`.
    ///
    /// - Otherwise, introduce a new [`LocalVariable`], and add an entry to
    ///   [`BlockContext::phis`] to arrange for `id`'s value to be spilled to
    ///   it. Then emit a fresh [`Load`] of that temporary variable for use in
    ///   `body_idx`'s block, and return its `Handle`.
    ///
    /// The SPIR-V domination rule ensures that the introduced [`LocalVariable`]
    /// will always have been initialized before it is used.
    ///
    /// `lookup` must be the [`LookupExpression`] for `id`.
    ///
    /// `body_idx` argument must be the index of the [`Body`] that hopes to use
    /// `id`'s [`Expression`].
    ///
    /// [`Expression`]: crate::Expression
    /// [`Handle`]: crate::Handle
    /// [`Block`]: crate::Block
    /// [body]: BlockContext::bodies
    /// [`LocalVariable`]: crate::LocalVariable
    /// [`Load`]: crate::Expression::Load
    fn get_expr_handle(
        &self,
        id: spirv::Word,
        lookup: &LookupExpression,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        body_idx: BodyIndex,
    ) -> Handle<crate::Expression> {
        // What `Body` was `id` defined in?
        let expr_body_idx = ctx
            .body_for_label
            .get(&lookup.block_id)
            .copied()
            .unwrap_or(0);

        // Don't need to do a load/store if the expression is in the main body
        // or if the expression is in the same body as where the query was
        // requested. The body_idx might actually not be the final one if a loop
        // or conditional occurs but in those cases we know that the new body
        // will be a subscope of the body that was passed so we can still reuse
        // the handle and not issue a load/store.
        if is_parent(body_idx, expr_body_idx, ctx) {
            lookup.handle
        } else {
            // Add a temporary variable of the same type which will be used to
            // store the original expression and used in the current block
            let ty = self.lookup_type[&lookup.type_id].handle;
            let local = ctx.local_arena.append(
                crate::LocalVariable {
                    name: None,
                    ty,
                    init: None,
                },
                crate::Span::default(),
            );

            block.extend(emitter.finish(ctx.expressions));
            let pointer = ctx.expressions.append(
                crate::Expression::LocalVariable(local),
                crate::Span::default(),
            );
            emitter.start(ctx.expressions);
            let expr = ctx
                .expressions
                .append(crate::Expression::Load { pointer }, crate::Span::default());

            // Add a slightly odd entry to the phi table, so that while `id`'s
            // `Expression` is still in scope, the usual phi processing will
            // spill its value to `local`, where we can find it later.
            //
            // This pretends that the block in which `id` is defined is the
            // predecessor of some other block with a phi in it that cites id as
            // one of its sources, and uses `local` as its variable. There is no
            // such phi, but nobody needs to know that.
            ctx.phis.push(PhiExpression {
                local,
                expressions: vec![(id, lookup.block_id)],
            });

            expr
        }
    }

    fn parse_expr_unary_op(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        op: crate::UnaryOperator,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let p_id = self.next()?;

        let p_lexp = self.lookup_expression.lookup(p_id)?;
        let handle = self.get_expr_handle(p_id, p_lexp, ctx, emitter, block, body_idx);

        let expr = crate::Expression::Unary { op, expr: handle };
        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, self.span_from_with_op(start)),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    fn parse_expr_binary_op(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        op: crate::BinaryOperator,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let p1_id = self.next()?;
        let p2_id = self.next()?;

        let p1_lexp = self.lookup_expression.lookup(p1_id)?;
        let left = self.get_expr_handle(p1_id, p1_lexp, ctx, emitter, block, body_idx);
        let p2_lexp = self.lookup_expression.lookup(p2_id)?;
        let right = self.get_expr_handle(p2_id, p2_lexp, ctx, emitter, block, body_idx);

        let expr = crate::Expression::Binary { op, left, right };
        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, self.span_from_with_op(start)),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    /// A more complicated version of the unary op,
    /// where we force the operand to have the same type as the result.
    fn parse_expr_unary_op_sign_adjusted(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        op: crate::UnaryOperator,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let p1_id = self.next()?;
        let span = self.span_from_with_op(start);

        let p1_lexp = self.lookup_expression.lookup(p1_id)?;
        let left = self.get_expr_handle(p1_id, p1_lexp, ctx, emitter, block, body_idx);

        let result_lookup_ty = self.lookup_type.lookup(result_type_id)?;
        let kind = ctx.module.types[result_lookup_ty.handle]
            .inner
            .scalar_kind()
            .unwrap();

        let expr = crate::Expression::Unary {
            op,
            expr: if p1_lexp.type_id == result_type_id {
                left
            } else {
                ctx.expressions.append(
                    crate::Expression::As {
                        expr: left,
                        kind,
                        convert: None,
                    },
                    span,
                )
            },
        };

        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, span),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    /// A more complicated version of the binary op,
    /// where we force the operand to have the same type as the result.
    /// This is mostly needed for "i++" and "i--" coming from GLSL.
    #[allow(clippy::too_many_arguments)]
    fn parse_expr_binary_op_sign_adjusted(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        op: crate::BinaryOperator,
        // For arithmetic operations, we need the sign of operands to match the result.
        // For boolean operations, however, the operands need to match the signs, but
        // result is always different - a boolean.
        anchor: SignAnchor,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let p1_id = self.next()?;
        let p2_id = self.next()?;
        let span = self.span_from_with_op(start);

        let p1_lexp = self.lookup_expression.lookup(p1_id)?;
        let left = self.get_expr_handle(p1_id, p1_lexp, ctx, emitter, block, body_idx);
        let p2_lexp = self.lookup_expression.lookup(p2_id)?;
        let right = self.get_expr_handle(p2_id, p2_lexp, ctx, emitter, block, body_idx);

        let expected_type_id = match anchor {
            SignAnchor::Result => result_type_id,
            SignAnchor::Operand => p1_lexp.type_id,
        };
        let expected_lookup_ty = self.lookup_type.lookup(expected_type_id)?;
        let kind = ctx.module.types[expected_lookup_ty.handle]
            .inner
            .scalar_kind()
            .unwrap();

        let expr = crate::Expression::Binary {
            op,
            left: if p1_lexp.type_id == expected_type_id {
                left
            } else {
                ctx.expressions.append(
                    crate::Expression::As {
                        expr: left,
                        kind,
                        convert: None,
                    },
                    span,
                )
            },
            right: if p2_lexp.type_id == expected_type_id {
                right
            } else {
                ctx.expressions.append(
                    crate::Expression::As {
                        expr: right,
                        kind,
                        convert: None,
                    },
                    span,
                )
            },
        };

        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, span),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    /// A version of the binary op where one or both of the arguments might need to be casted to a
    /// specific integer kind (unsigned or signed), used for operations like OpINotEqual or
    /// OpUGreaterThan.
    #[allow(clippy::too_many_arguments)]
    fn parse_expr_int_comparison(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        op: crate::BinaryOperator,
        kind: crate::ScalarKind,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let p1_id = self.next()?;
        let p2_id = self.next()?;
        let span = self.span_from_with_op(start);

        let p1_lexp = self.lookup_expression.lookup(p1_id)?;
        let left = self.get_expr_handle(p1_id, p1_lexp, ctx, emitter, block, body_idx);
        let p1_lookup_ty = self.lookup_type.lookup(p1_lexp.type_id)?;
        let p1_kind = ctx.module.types[p1_lookup_ty.handle]
            .inner
            .scalar_kind()
            .unwrap();
        let p2_lexp = self.lookup_expression.lookup(p2_id)?;
        let right = self.get_expr_handle(p2_id, p2_lexp, ctx, emitter, block, body_idx);
        let p2_lookup_ty = self.lookup_type.lookup(p2_lexp.type_id)?;
        let p2_kind = ctx.module.types[p2_lookup_ty.handle]
            .inner
            .scalar_kind()
            .unwrap();

        let expr = crate::Expression::Binary {
            op,
            left: if p1_kind == kind {
                left
            } else {
                ctx.expressions.append(
                    crate::Expression::As {
                        expr: left,
                        kind,
                        convert: None,
                    },
                    span,
                )
            },
            right: if p2_kind == kind {
                right
            } else {
                ctx.expressions.append(
                    crate::Expression::As {
                        expr: right,
                        kind,
                        convert: None,
                    },
                    span,
                )
            },
        };

        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, span),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    fn parse_expr_shift_op(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        op: crate::BinaryOperator,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let p1_id = self.next()?;
        let p2_id = self.next()?;

        let span = self.span_from_with_op(start);

        let p1_lexp = self.lookup_expression.lookup(p1_id)?;
        let left = self.get_expr_handle(p1_id, p1_lexp, ctx, emitter, block, body_idx);
        let p2_lexp = self.lookup_expression.lookup(p2_id)?;
        let p2_handle = self.get_expr_handle(p2_id, p2_lexp, ctx, emitter, block, body_idx);
        // convert the shift to Uint
        let right = ctx.expressions.append(
            crate::Expression::As {
                expr: p2_handle,
                kind: crate::ScalarKind::Uint,
                convert: None,
            },
            span,
        );

        let expr = crate::Expression::Binary { op, left, right };
        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, span),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    fn parse_expr_derivative(
        &mut self,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        (axis, ctrl): (crate::DerivativeAxis, crate::DerivativeControl),
    ) -> Result<(), Error> {
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let arg_id = self.next()?;

        let arg_lexp = self.lookup_expression.lookup(arg_id)?;
        let arg_handle = self.get_expr_handle(arg_id, arg_lexp, ctx, emitter, block, body_idx);

        let expr = crate::Expression::Derivative {
            axis,
            ctrl,
            expr: arg_handle,
        };
        self.lookup_expression.insert(
            result_id,
            LookupExpression {
                handle: ctx.expressions.append(expr, self.span_from_with_op(start)),
                type_id: result_type_id,
                block_id,
            },
        );
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn insert_composite(
        &self,
        root_expr: Handle<crate::Expression>,
        root_type_id: spirv::Word,
        object_expr: Handle<crate::Expression>,
        selections: &[spirv::Word],
        type_arena: &UniqueArena<crate::Type>,
        expressions: &mut Arena<crate::Expression>,
        span: crate::Span,
    ) -> Result<Handle<crate::Expression>, Error> {
        let selection = match selections.first() {
            Some(&index) => index,
            None => return Ok(object_expr),
        };
        let root_span = expressions.get_span(root_expr);
        let root_lookup = self.lookup_type.lookup(root_type_id)?;

        let (count, child_type_id) = match type_arena[root_lookup.handle].inner {
            crate::TypeInner::Struct { ref members, .. } => {
                let child_member = self
                    .lookup_member
                    .get(&(root_lookup.handle, selection))
                    .ok_or(Error::InvalidAccessType(root_type_id))?;
                (members.len(), child_member.type_id)
            }
            crate::TypeInner::Array { size, .. } => {
                let size = match size {
                    crate::ArraySize::Constant(size) => size.get(),
                    crate::ArraySize::Pending(_) => {
                        unreachable!();
                    }
                    // A runtime sized array is not a composite type
                    crate::ArraySize::Dynamic => {
                        return Err(Error::InvalidAccessType(root_type_id))
                    }
                };

                let child_type_id = root_lookup
                    .base_id
                    .ok_or(Error::InvalidAccessType(root_type_id))?;

                (size as usize, child_type_id)
            }
            crate::TypeInner::Vector { size, .. }
            | crate::TypeInner::Matrix { columns: size, .. } => {
                let child_type_id = root_lookup
                    .base_id
                    .ok_or(Error::InvalidAccessType(root_type_id))?;
                (size as usize, child_type_id)
            }
            _ => return Err(Error::InvalidAccessType(root_type_id)),
        };

        let mut components = Vec::with_capacity(count);
        for index in 0..count as u32 {
            let expr = expressions.append(
                crate::Expression::AccessIndex {
                    base: root_expr,
                    index,
                },
                if index == selection { span } else { root_span },
            );
            components.push(expr);
        }
        components[selection as usize] = self.insert_composite(
            components[selection as usize],
            child_type_id,
            object_expr,
            &selections[1..],
            type_arena,
            expressions,
            span,
        )?;

        Ok(expressions.append(
            crate::Expression::Compose {
                ty: root_lookup.handle,
                components,
            },
            span,
        ))
    }

    /// Return the Naga [`Expression`] for `pointer_id`, and its referent [`Type`].
    ///
    /// Return a [`Handle`] for a Naga [`Expression`] that holds the value of
    /// the SPIR-V instruction `pointer_id`, along with the [`Type`] to which it
    /// is a pointer.
    ///
    /// This may entail spilling `pointer_id`'s value to a temporary:
    /// see [`get_expr_handle`]'s documentation.
    ///
    /// [`Expression`]: crate::Expression
    /// [`Type`]: crate::Type
    /// [`Handle`]: crate::Handle
    /// [`get_expr_handle`]: Frontend::get_expr_handle
    fn get_exp_and_base_ty_handles(
        &self,
        pointer_id: spirv::Word,
        ctx: &mut BlockContext,
        emitter: &mut crate::proc::Emitter,
        block: &mut crate::Block,
        body_idx: usize,
    ) -> Result<(Handle<crate::Expression>, Handle<crate::Type>), Error> {
        log::trace!("\t\t\tlooking up pointer expr {:?}", pointer_id);
        let p_lexp_handle;
        let p_lexp_ty_id;
        {
            let lexp = self.lookup_expression.lookup(pointer_id)?;
            p_lexp_handle = self.get_expr_handle(pointer_id, lexp, ctx, emitter, block, body_idx);
            p_lexp_ty_id = lexp.type_id;
        };

        log::trace!("\t\t\tlooking up pointer type {pointer_id:?}");
        let p_ty = self.lookup_type.lookup(p_lexp_ty_id)?;
        let p_ty_base_id = p_ty.base_id.ok_or(Error::InvalidAccessType(p_lexp_ty_id))?;

        log::trace!("\t\t\tlooking up pointer base type {p_ty_base_id:?} of {p_ty:?}");
        let p_base_ty = self.lookup_type.lookup(p_ty_base_id)?;

        Ok((p_lexp_handle, p_base_ty.handle))
    }

    #[allow(clippy::too_many_arguments)]
    fn parse_atomic_expr_with_value(
        &mut self,
        inst: Instruction,
        emitter: &mut crate::proc::Emitter,
        ctx: &mut BlockContext,
        block: &mut crate::Block,
        block_id: spirv::Word,
        body_idx: usize,
        atomic_function: crate::AtomicFunction,
    ) -> Result<(), Error> {
        inst.expect(7)?;
        let start = self.data_offset;
        let result_type_id = self.next()?;
        let result_id = self.next()?;
        let pointer_id = self.next()?;
        let _scope_id = self.next()?;
        let _memory_semantics_id = self.next()?;
        let value_id = self.next()?;
        let span = self.span_from_with_op(start);

        let (p_lexp_handle, p_base_ty_handle) =
            self.get_exp_and_base_ty_handles(pointer_id, ctx, emitter, block, body_idx)?;

        log::trace!("\t\t\tlooking up value expr {value_id:?}");
        let v_lexp_handle = self.lookup_expression.lookup(value_id)?.handle;

        block.extend(emitter.finish(ctx.expressions));
        // Create an expression for our result
        let r_lexp_handle = {
            let expr = crate::Expression::AtomicResult {
                ty: p_base_ty_handle,
                comparison: false,
            };
            let handle = ctx.expressions.append(expr, span);
            self.lookup_expression.insert(
                result_id,
                LookupExpression {
                    handle,
                    type_id: result_type_id,
                    block_id,
                },
            );
            handle
        };
        emitter.start(ctx.expressions);

        // Create a statement for the op itself
        let stmt = crate::Statement::Atomic {
            pointer: p_lexp_handle,
            fun: atomic_function,
            value: v_lexp_handle,
            result: Some(r_lexp_handle),
        };
        block.push(stmt, span);

        // Store any associated global variables so we can upgrade their types later
        self.record_atomic_access(ctx, p_lexp_handle)?;

        Ok(())
    }

    /// Add the next SPIR-V block's contents to `block_ctx`.
    ///
    /// Except for the function's entry block, `block_id` should be the label of
    /// a block we've seen mentioned before, with an entry in
    /// `block_ctx.body_for_label` to tell us which `Body` it contributes to.
    fn next_block(&mut self, block_id: spirv::Word, ctx: &mut BlockContext) -> Result<(), Error> {
        // Extend `body` with the correct form for a branch to `target`.
        fn merger(body: &mut Body, target: &MergeBlockInformation) {
            body.data.push(match *target {
                MergeBlockInformation::LoopContinue => BodyFragment::Continue,
                MergeBlockInformation::LoopMerge | MergeBlockInformation::SwitchMerge => {
                    BodyFragment::Break
                }

                // Finishing a selection merge means just falling off the end of
                // the `accept` or `reject` block of the `If` statement.
                MergeBlockInformation::SelectionMerge => return,
            })
        }

        let mut emitter = crate::proc::Emitter::default();
        emitter.start(ctx.expressions);

        // Find the `Body` to which this block contributes.
        //
        // If this is some SPIR-V structured control flow construct's merge
        // block, then `body_idx` will refer to the same `Body` as the header,
        // so that we simply pick up accumulating the `Body` where the header
        // left off. Each of the statements in a block dominates the next, so
        // we're sure to encounter their SPIR-V blocks in order, ensuring that
        // the `Body` will be assembled in the proper order.
        //
        // Note that, unlike every other kind of SPIR-V block, we don't know the
        // function's first block's label in advance. Thus, we assume that if
        // this block has no entry in `ctx.body_for_label`, it must be the
        // function's first block. This always has body index zero.
        let mut body_idx = *ctx.body_for_label.entry(block_id).or_default();

        // The Naga IR block this call builds. This will end up as
        // `ctx.blocks[&block_id]`, and `ctx.bodies[body_idx]` will refer to it
        // via a `BodyFragment::BlockId`.
        let mut block = crate::Block::new();

        // Stores the merge block as defined by a `OpSelectionMerge` otherwise is `None`
        //
        // This is used in `OpSwitch` to promote the `MergeBlockInformation` from
        // `SelectionMerge` to `SwitchMerge` to allow `Break`s this isn't desirable for
        // `LoopMerge`s because otherwise `Continue`s wouldn't be allowed
        let mut selection_merge_block = None;

        macro_rules! get_expr_handle {
            ($id:expr, $lexp:expr) => {
                self.get_expr_handle($id, $lexp, ctx, &mut emitter, &mut block, body_idx)
            };
        }
        macro_rules! parse_expr_op {
            ($op:expr, BINARY) => {
                self.parse_expr_binary_op(ctx, &mut emitter, &mut block, block_id, body_idx, $op)
            };

            ($op:expr, SHIFT) => {
                self.parse_expr_shift_op(ctx, &mut emitter, &mut block, block_id, body_idx, $op)
            };
            ($op:expr, UNARY) => {
                self.parse_expr_unary_op(ctx, &mut emitter, &mut block, block_id, body_idx, $op)
            };
            ($axis:expr, $ctrl:expr, DERIVATIVE) => {
                self.parse_expr_derivative(
                    ctx,
                    &mut emitter,
                    &mut block,
                    block_id,
                    body_idx,
                    ($axis, $ctrl),
                )
            };
        }

        let terminator = loop {
            use spirv::Op;
            let start = self.data_offset;
            let inst = self.next_inst()?;
            let span = crate::Span::from(start..(start + 4 * (inst.wc as usize)));
            log::debug!("\t\t{:?} [{}]", inst.op, inst.wc);

            match inst.op {
                Op::Line => {
                    inst.expect(4)?;
                    let _file_id = self.next()?;
                    let _row_id = self.next()?;
                    let _col_id = self.next()?;
                }
                Op::NoLine => inst.expect(1)?,
                Op::Undef => {
                    inst.expect(3)?;
                    let type_id = self.next()?;
                    let id = self.next()?;
                    let type_lookup = self.lookup_type.lookup(type_id)?;
                    let ty = type_lookup.handle;

                    self.lookup_expression.insert(
                        id,
                        LookupExpression {
                            handle: ctx
                                .expressions
                                .append(crate::Expression::ZeroValue(ty), span),
                            type_id,
                            block_id,
                        },
                    );
                }
                Op::Variable => {
                    inst.expect_at_least(4)?;
                    block.extend(emitter.finish(ctx.expressions));

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let _storage_class = self.next()?;
                    let init = if inst.wc > 4 {
                        inst.expect(5)?;
                        let init_id = self.next()?;
                        let lconst = self.lookup_constant.lookup(init_id)?;
                        Some(ctx.expressions.append(lconst.inner.to_expr(), span))
                    } else {
                        None
                    };

                    let name = self
                        .future_decor
                        .remove(&result_id)
                        .and_then(|decor| decor.name);
                    if let Some(ref name) = name {
                        log::debug!("\t\t\tid={} name={}", result_id, name);
                    }
                    let lookup_ty = self.lookup_type.lookup(result_type_id)?;
                    let var_handle = ctx.local_arena.append(
                        crate::LocalVariable {
                            name,
                            ty: match ctx.module.types[lookup_ty.handle].inner {
                                crate::TypeInner::Pointer { base, .. } => base,
                                _ => lookup_ty.handle,
                            },
                            init,
                        },
                        span,
                    );

                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx
                                .expressions
                                .append(crate::Expression::LocalVariable(var_handle), span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                    emitter.start(ctx.expressions);
                }
                Op::Phi => {
                    inst.expect_at_least(3)?;
                    block.extend(emitter.finish(ctx.expressions));

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;

                    let name = format!("phi_{result_id}");
                    let local = ctx.local_arena.append(
                        crate::LocalVariable {
                            name: Some(name),
                            ty: self.lookup_type.lookup(result_type_id)?.handle,
                            init: None,
                        },
                        self.span_from(start),
                    );
                    let pointer = ctx
                        .expressions
                        .append(crate::Expression::LocalVariable(local), span);

                    let in_count = (inst.wc - 3) / 2;
                    let mut phi = PhiExpression {
                        local,
                        expressions: Vec::with_capacity(in_count as usize),
                    };
                    for _ in 0..in_count {
                        let expr = self.next()?;
                        let block = self.next()?;
                        phi.expressions.push((expr, block));
                    }

                    ctx.phis.push(phi);
                    emitter.start(ctx.expressions);

                    // Associate the lookup with an actual value, which is emitted
                    // into the current block.
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx
                                .expressions
                                .append(crate::Expression::Load { pointer }, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::AccessChain | Op::InBoundsAccessChain => {
                    struct AccessExpression {
                        base_handle: Handle<crate::Expression>,
                        type_id: spirv::Word,
                        load_override: Option<LookupLoadOverride>,
                    }

                    inst.expect_at_least(4)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let base_id = self.next()?;
                    log::trace!("\t\t\tlooking up expr {:?}", base_id);

                    let mut acex = {
                        let lexp = self.lookup_expression.lookup(base_id)?;
                        let lty = self.lookup_type.lookup(lexp.type_id)?;

                        // HACK `OpAccessChain` and `OpInBoundsAccessChain`
                        // require for the result type to be a pointer, but if
                        // we're given a pointer to an image / sampler, it will
                        // be *already* dereferenced, since we do that early
                        // during `parse_type_pointer()`.
                        //
                        // This can happen only through `BindingArray`, since
                        // that's the only case where one can obtain a pointer
                        // to an image / sampler, and so let's match on that:
                        let dereference = match ctx.module.types[lty.handle].inner {
                            crate::TypeInner::BindingArray { .. } => false,
                            _ => true,
                        };

                        let type_id = if dereference {
                            lty.base_id.ok_or(Error::InvalidAccessType(lexp.type_id))?
                        } else {
                            lexp.type_id
                        };

                        AccessExpression {
                            base_handle: get_expr_handle!(base_id, lexp),
                            type_id,
                            load_override: self.lookup_load_override.get(&base_id).cloned(),
                        }
                    };

                    for _ in 4..inst.wc {
                        let access_id = self.next()?;
                        log::trace!("\t\t\tlooking up index expr {:?}", access_id);
                        let index_expr = self.lookup_expression.lookup(access_id)?.clone();
                        let index_expr_handle = get_expr_handle!(access_id, &index_expr);
                        let index_expr_data = &ctx.expressions[index_expr.handle];
                        let index_maybe = match *index_expr_data {
                            crate::Expression::Constant(const_handle) => Some(
                                ctx.gctx()
                                    .eval_expr_to_u32(ctx.module.constants[const_handle].init)
                                    .map_err(|_| {
                                        Error::InvalidAccess(crate::Expression::Constant(
                                            const_handle,
                                        ))
                                    })?,
                            ),
                            _ => None,
                        };

                        log::trace!("\t\t\tlooking up type {:?}", acex.type_id);
                        let type_lookup = self.lookup_type.lookup(acex.type_id)?;
                        let ty = &ctx.module.types[type_lookup.handle];
                        acex = match ty.inner {
                            // can only index a struct with a constant
                            crate::TypeInner::Struct { ref members, .. } => {
                                let index = index_maybe
                                    .ok_or_else(|| Error::InvalidAccess(index_expr_data.clone()))?;

                                let lookup_member = self
                                    .lookup_member
                                    .get(&(type_lookup.handle, index))
                                    .ok_or(Error::InvalidAccessType(acex.type_id))?;
                                let base_handle = ctx.expressions.append(
                                    crate::Expression::AccessIndex {
                                        base: acex.base_handle,
                                        index,
                                    },
                                    span,
                                );

                                if let Some(crate::Binding::BuiltIn(built_in)) =
                                    members[index as usize].binding
                                {
                                    self.gl_per_vertex_builtin_access.insert(built_in);
                                }

                                AccessExpression {
                                    base_handle,
                                    type_id: lookup_member.type_id,
                                    load_override: if lookup_member.row_major {
                                        debug_assert!(acex.load_override.is_none());
                                        let sub_type_lookup =
                                            self.lookup_type.lookup(lookup_member.type_id)?;
                                        Some(match ctx.module.types[sub_type_lookup.handle].inner {
                                            // load it transposed, to match column major expectations
                                            crate::TypeInner::Matrix { .. } => {
                                                let loaded = ctx.expressions.append(
                                                    crate::Expression::Load {
                                                        pointer: base_handle,
                                                    },
                                                    span,
                                                );
                                                let transposed = ctx.expressions.append(
                                                    crate::Expression::Math {
                                                        fun: crate::MathFunction::Transpose,
                                                        arg: loaded,
                                                        arg1: None,
                                                        arg2: None,
                                                        arg3: None,
                                                    },
                                                    span,
                                                );
                                                LookupLoadOverride::Loaded(transposed)
                                            }
                                            _ => LookupLoadOverride::Pending,
                                        })
                                    } else {
                                        None
                                    },
                                }
                            }
                            crate::TypeInner::Matrix { .. } => {
                                let load_override = match acex.load_override {
                                    // We are indexing inside a row-major matrix
                                    Some(LookupLoadOverride::Loaded(load_expr)) => {
                                        let index = index_maybe.ok_or_else(|| {
                                            Error::InvalidAccess(index_expr_data.clone())
                                        })?;
                                        let sub_handle = ctx.expressions.append(
                                            crate::Expression::AccessIndex {
                                                base: load_expr,
                                                index,
                                            },
                                            span,
                                        );
                                        Some(LookupLoadOverride::Loaded(sub_handle))
                                    }
                                    _ => None,
                                };
                                let sub_expr = match index_maybe {
                                    Some(index) => crate::Expression::AccessIndex {
                                        base: acex.base_handle,
                                        index,
                                    },
                                    None => crate::Expression::Access {
                                        base: acex.base_handle,
                                        index: index_expr_handle,
                                    },
                                };
                                AccessExpression {
                                    base_handle: ctx.expressions.append(sub_expr, span),
                                    type_id: type_lookup
                                        .base_id
                                        .ok_or(Error::InvalidAccessType(acex.type_id))?,
                                    load_override,
                                }
                            }
                            // This must be a vector or an array.
                            _ => {
                                let base_handle = ctx.expressions.append(
                                    crate::Expression::Access {
                                        base: acex.base_handle,
                                        index: index_expr_handle,
                                    },
                                    span,
                                );
                                let load_override = match acex.load_override {
                                    // If there is a load override in place, then we always end up
                                    // with a side-loaded value here.
                                    Some(lookup_load_override) => {
                                        let sub_expr = match lookup_load_override {
                                            // We must be indexing into the array of row-major matrices.
                                            // Let's load the result of indexing and transpose it.
                                            LookupLoadOverride::Pending => {
                                                let loaded = ctx.expressions.append(
                                                    crate::Expression::Load {
                                                        pointer: base_handle,
                                                    },
                                                    span,
                                                );
                                                ctx.expressions.append(
                                                    crate::Expression::Math {
                                                        fun: crate::MathFunction::Transpose,
                                                        arg: loaded,
                                                        arg1: None,
                                                        arg2: None,
                                                        arg3: None,
                                                    },
                                                    span,
                                                )
                                            }
                                            // We are indexing inside a row-major matrix.
                                            LookupLoadOverride::Loaded(load_expr) => {
                                                ctx.expressions.append(
                                                    crate::Expression::Access {
                                                        base: load_expr,
                                                        index: index_expr_handle,
                                                    },
                                                    span,
                                                )
                                            }
                                        };
                                        Some(LookupLoadOverride::Loaded(sub_expr))
                                    }
                                    None => None,
                                };
                                AccessExpression {
                                    base_handle,
                                    type_id: type_lookup
                                        .base_id
                                        .ok_or(Error::InvalidAccessType(acex.type_id))?,
                                    load_override,
                                }
                            }
                        };
                    }

                    if let Some(load_expr) = acex.load_override {
                        self.lookup_load_override.insert(result_id, load_expr);
                    }
                    let lookup_expression = LookupExpression {
                        handle: acex.base_handle,
                        type_id: result_type_id,
                        block_id,
                    };
                    self.lookup_expression.insert(result_id, lookup_expression);
                }
                Op::VectorExtractDynamic => {
                    inst.expect(5)?;

                    let result_type_id = self.next()?;
                    let id = self.next()?;
                    let composite_id = self.next()?;
                    let index_id = self.next()?;

                    let root_lexp = self.lookup_expression.lookup(composite_id)?;
                    let root_handle = get_expr_handle!(composite_id, root_lexp);
                    let root_type_lookup = self.lookup_type.lookup(root_lexp.type_id)?;
                    let index_lexp = self.lookup_expression.lookup(index_id)?;
                    let index_handle = get_expr_handle!(index_id, index_lexp);
                    let index_type = self.lookup_type.lookup(index_lexp.type_id)?.handle;

                    let num_components = match ctx.module.types[root_type_lookup.handle].inner {
                        crate::TypeInner::Vector { size, .. } => size as u32,
                        _ => return Err(Error::InvalidVectorType(root_type_lookup.handle)),
                    };

                    let mut make_index = |ctx: &mut BlockContext, index: u32| {
                        make_index_literal(
                            ctx,
                            index,
                            &mut block,
                            &mut emitter,
                            index_type,
                            index_lexp.type_id,
                            span,
                        )
                    };

                    let index_expr = make_index(ctx, 0)?;
                    let mut handle = ctx.expressions.append(
                        crate::Expression::Access {
                            base: root_handle,
                            index: index_expr,
                        },
                        span,
                    );
                    for index in 1..num_components {
                        let index_expr = make_index(ctx, index)?;
                        let access_expr = ctx.expressions.append(
                            crate::Expression::Access {
                                base: root_handle,
                                index: index_expr,
                            },
                            span,
                        );
                        let cond = ctx.expressions.append(
                            crate::Expression::Binary {
                                op: crate::BinaryOperator::Equal,
                                left: index_expr,
                                right: index_handle,
                            },
                            span,
                        );
                        handle = ctx.expressions.append(
                            crate::Expression::Select {
                                condition: cond,
                                accept: access_expr,
                                reject: handle,
                            },
                            span,
                        );
                    }

                    self.lookup_expression.insert(
                        id,
                        LookupExpression {
                            handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::VectorInsertDynamic => {
                    inst.expect(6)?;

                    let result_type_id = self.next()?;
                    let id = self.next()?;
                    let composite_id = self.next()?;
                    let object_id = self.next()?;
                    let index_id = self.next()?;

                    let object_lexp = self.lookup_expression.lookup(object_id)?;
                    let object_handle = get_expr_handle!(object_id, object_lexp);
                    let root_lexp = self.lookup_expression.lookup(composite_id)?;
                    let root_handle = get_expr_handle!(composite_id, root_lexp);
                    let root_type_lookup = self.lookup_type.lookup(root_lexp.type_id)?;
                    let index_lexp = self.lookup_expression.lookup(index_id)?;
                    let index_handle = get_expr_handle!(index_id, index_lexp);
                    let index_type = self.lookup_type.lookup(index_lexp.type_id)?.handle;

                    let num_components = match ctx.module.types[root_type_lookup.handle].inner {
                        crate::TypeInner::Vector { size, .. } => size as u32,
                        _ => return Err(Error::InvalidVectorType(root_type_lookup.handle)),
                    };

                    let mut components = Vec::with_capacity(num_components as usize);
                    for index in 0..num_components {
                        let index_expr = make_index_literal(
                            ctx,
                            index,
                            &mut block,
                            &mut emitter,
                            index_type,
                            index_lexp.type_id,
                            span,
                        )?;
                        let access_expr = ctx.expressions.append(
                            crate::Expression::Access {
                                base: root_handle,
                                index: index_expr,
                            },
                            span,
                        );
                        let cond = ctx.expressions.append(
                            crate::Expression::Binary {
                                op: crate::BinaryOperator::Equal,
                                left: index_expr,
                                right: index_handle,
                            },
                            span,
                        );
                        let handle = ctx.expressions.append(
                            crate::Expression::Select {
                                condition: cond,
                                accept: object_handle,
                                reject: access_expr,
                            },
                            span,
                        );
                        components.push(handle);
                    }
                    let handle = ctx.expressions.append(
                        crate::Expression::Compose {
                            ty: root_type_lookup.handle,
                            components,
                        },
                        span,
                    );

                    self.lookup_expression.insert(
                        id,
                        LookupExpression {
                            handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::CompositeExtract => {
                    inst.expect_at_least(4)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let base_id = self.next()?;
                    log::trace!("\t\t\tlooking up expr {:?}", base_id);
                    let mut lexp = self.lookup_expression.lookup(base_id)?.clone();
                    lexp.handle = get_expr_handle!(base_id, &lexp);
                    for _ in 4..inst.wc {
                        let index = self.next()?;
                        log::trace!("\t\t\tlooking up type {:?}", lexp.type_id);
                        let type_lookup = self.lookup_type.lookup(lexp.type_id)?;
                        let type_id = match ctx.module.types[type_lookup.handle].inner {
                            crate::TypeInner::Struct { .. } => {
                                self.lookup_member
                                    .get(&(type_lookup.handle, index))
                                    .ok_or(Error::InvalidAccessType(lexp.type_id))?
                                    .type_id
                            }
                            crate::TypeInner::Array { .. }
                            | crate::TypeInner::Vector { .. }
                            | crate::TypeInner::Matrix { .. } => type_lookup
                                .base_id
                                .ok_or(Error::InvalidAccessType(lexp.type_id))?,
                            ref other => {
                                log::warn!("composite type {:?}", other);
                                return Err(Error::UnsupportedType(type_lookup.handle));
                            }
                        };
                        lexp = LookupExpression {
                            handle: ctx.expressions.append(
                                crate::Expression::AccessIndex {
                                    base: lexp.handle,
                                    index,
                                },
                                span,
                            ),
                            type_id,
                            block_id,
                        };
                    }

                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: lexp.handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::CompositeInsert => {
                    inst.expect_at_least(5)?;

                    let result_type_id = self.next()?;
                    let id = self.next()?;
                    let object_id = self.next()?;
                    let composite_id = self.next()?;
                    let mut selections = Vec::with_capacity(inst.wc as usize - 5);
                    for _ in 5..inst.wc {
                        selections.push(self.next()?);
                    }

                    let object_lexp = self.lookup_expression.lookup(object_id)?.clone();
                    let object_handle = get_expr_handle!(object_id, &object_lexp);
                    let root_lexp = self.lookup_expression.lookup(composite_id)?.clone();
                    let root_handle = get_expr_handle!(composite_id, &root_lexp);
                    let handle = self.insert_composite(
                        root_handle,
                        result_type_id,
                        object_handle,
                        &selections,
                        &ctx.module.types,
                        ctx.expressions,
                        span,
                    )?;

                    self.lookup_expression.insert(
                        id,
                        LookupExpression {
                            handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::CompositeConstruct => {
                    inst.expect_at_least(3)?;

                    let result_type_id = self.next()?;
                    let id = self.next()?;
                    let mut components = Vec::with_capacity(inst.wc as usize - 2);
                    for _ in 3..inst.wc {
                        let comp_id = self.next()?;
                        log::trace!("\t\t\tlooking up expr {:?}", comp_id);
                        let lexp = self.lookup_expression.lookup(comp_id)?;
                        let handle = get_expr_handle!(comp_id, lexp);
                        components.push(handle);
                    }
                    let ty = self.lookup_type.lookup(result_type_id)?.handle;
                    let first = components[0];
                    let expr = match ctx.module.types[ty].inner {
                        // this is an optimization to detect the splat
                        crate::TypeInner::Vector { size, .. }
                            if components.len() == size as usize
                                && components[1..].iter().all(|&c| c == first) =>
                        {
                            crate::Expression::Splat { size, value: first }
                        }
                        _ => crate::Expression::Compose { ty, components },
                    };
                    self.lookup_expression.insert(
                        id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::Load => {
                    inst.expect_at_least(4)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let pointer_id = self.next()?;
                    if inst.wc != 4 {
                        inst.expect(5)?;
                        let _memory_access = self.next()?;
                    }

                    let base_lexp = self.lookup_expression.lookup(pointer_id)?;
                    let base_handle = get_expr_handle!(pointer_id, base_lexp);
                    let type_lookup = self.lookup_type.lookup(base_lexp.type_id)?;
                    let handle = match ctx.module.types[type_lookup.handle].inner {
                        crate::TypeInner::Image { .. } | crate::TypeInner::Sampler { .. } => {
                            base_handle
                        }
                        _ => match self.lookup_load_override.get(&pointer_id) {
                            Some(&LookupLoadOverride::Loaded(handle)) => handle,
                            //Note: we aren't handling `LookupLoadOverride::Pending` properly here
                            _ => ctx.expressions.append(
                                crate::Expression::Load {
                                    pointer: base_handle,
                                },
                                span,
                            ),
                        },
                    };

                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::Store => {
                    inst.expect_at_least(3)?;

                    let pointer_id = self.next()?;
                    let value_id = self.next()?;
                    if inst.wc != 3 {
                        inst.expect(4)?;
                        let _memory_access = self.next()?;
                    }
                    let base_expr = self.lookup_expression.lookup(pointer_id)?;
                    let base_handle = get_expr_handle!(pointer_id, base_expr);
                    let value_expr = self.lookup_expression.lookup(value_id)?;
                    let value_handle = get_expr_handle!(value_id, value_expr);

                    block.extend(emitter.finish(ctx.expressions));
                    block.push(
                        crate::Statement::Store {
                            pointer: base_handle,
                            value: value_handle,
                        },
                        span,
                    );
                    emitter.start(ctx.expressions);
                }
                // Arithmetic Instructions +, -, *, /, %
                Op::SNegate | Op::FNegate => {
                    inst.expect(4)?;
                    self.parse_expr_unary_op_sign_adjusted(
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                        crate::UnaryOperator::Negate,
                    )?;
                }
                Op::IAdd
                | Op::ISub
                | Op::IMul
                | Op::BitwiseOr
                | Op::BitwiseXor
                | Op::BitwiseAnd
                | Op::SDiv
                | Op::SRem => {
                    inst.expect(5)?;
                    let operator = map_binary_operator(inst.op)?;
                    self.parse_expr_binary_op_sign_adjusted(
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                        operator,
                        SignAnchor::Result,
                    )?;
                }
                Op::IEqual | Op::INotEqual => {
                    inst.expect(5)?;
                    let operator = map_binary_operator(inst.op)?;
                    self.parse_expr_binary_op_sign_adjusted(
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                        operator,
                        SignAnchor::Operand,
                    )?;
                }
                Op::FAdd => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::Add, BINARY)?;
                }
                Op::FSub => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::Subtract, BINARY)?;
                }
                Op::FMul => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::Multiply, BINARY)?;
                }
                Op::UDiv | Op::FDiv => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::Divide, BINARY)?;
                }
                Op::UMod | Op::FRem => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::Modulo, BINARY)?;
                }
                Op::SMod => {
                    inst.expect(5)?;

                    // x - y * int(floor(float(x) / float(y)))

                    let start = self.data_offset;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let p1_id = self.next()?;
                    let p2_id = self.next()?;
                    let span = self.span_from_with_op(start);

                    let p1_lexp = self.lookup_expression.lookup(p1_id)?;
                    let left = self.get_expr_handle(
                        p1_id,
                        p1_lexp,
                        ctx,
                        &mut emitter,
                        &mut block,
                        body_idx,
                    );
                    let p2_lexp = self.lookup_expression.lookup(p2_id)?;
                    let right = self.get_expr_handle(
                        p2_id,
                        p2_lexp,
                        ctx,
                        &mut emitter,
                        &mut block,
                        body_idx,
                    );

                    let result_ty = self.lookup_type.lookup(result_type_id)?;
                    let inner = &ctx.module.types[result_ty.handle].inner;
                    let kind = inner.scalar_kind().unwrap();
                    let size = inner.size(ctx.gctx()) as u8;

                    let left_cast = ctx.expressions.append(
                        crate::Expression::As {
                            expr: left,
                            kind: crate::ScalarKind::Float,
                            convert: Some(size),
                        },
                        span,
                    );
                    let right_cast = ctx.expressions.append(
                        crate::Expression::As {
                            expr: right,
                            kind: crate::ScalarKind::Float,
                            convert: Some(size),
                        },
                        span,
                    );
                    let div = ctx.expressions.append(
                        crate::Expression::Binary {
                            op: crate::BinaryOperator::Divide,
                            left: left_cast,
                            right: right_cast,
                        },
                        span,
                    );
                    let floor = ctx.expressions.append(
                        crate::Expression::Math {
                            fun: crate::MathFunction::Floor,
                            arg: div,
                            arg1: None,
                            arg2: None,
                            arg3: None,
                        },
                        span,
                    );
                    let cast = ctx.expressions.append(
                        crate::Expression::As {
                            expr: floor,
                            kind,
                            convert: Some(size),
                        },
                        span,
                    );
                    let mult = ctx.expressions.append(
                        crate::Expression::Binary {
                            op: crate::BinaryOperator::Multiply,
                            left: cast,
                            right,
                        },
                        span,
                    );
                    let sub = ctx.expressions.append(
                        crate::Expression::Binary {
                            op: crate::BinaryOperator::Subtract,
                            left,
                            right: mult,
                        },
                        span,
                    );
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: sub,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::FMod => {
                    inst.expect(5)?;

                    // x - y * floor(x / y)

                    let start = self.data_offset;
                    let span = self.span_from_with_op(start);

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let p1_id = self.next()?;
                    let p2_id = self.next()?;

                    let p1_lexp = self.lookup_expression.lookup(p1_id)?;
                    let left = self.get_expr_handle(
                        p1_id,
                        p1_lexp,
                        ctx,
                        &mut emitter,
                        &mut block,
                        body_idx,
                    );
                    let p2_lexp = self.lookup_expression.lookup(p2_id)?;
                    let right = self.get_expr_handle(
                        p2_id,
                        p2_lexp,
                        ctx,
                        &mut emitter,
                        &mut block,
                        body_idx,
                    );

                    let div = ctx.expressions.append(
                        crate::Expression::Binary {
                            op: crate::BinaryOperator::Divide,
                            left,
                            right,
                        },
                        span,
                    );
                    let floor = ctx.expressions.append(
                        crate::Expression::Math {
                            fun: crate::MathFunction::Floor,
                            arg: div,
                            arg1: None,
                            arg2: None,
                            arg3: None,
                        },
                        span,
                    );
                    let mult = ctx.expressions.append(
                        crate::Expression::Binary {
                            op: crate::BinaryOperator::Multiply,
                            left: floor,
                            right,
                        },
                        span,
                    );
                    let sub = ctx.expressions.append(
                        crate::Expression::Binary {
                            op: crate::BinaryOperator::Subtract,
                            left,
                            right: mult,
                        },
                        span,
                    );
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: sub,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::VectorTimesScalar
                | Op::VectorTimesMatrix
                | Op::MatrixTimesScalar
                | Op::MatrixTimesVector
                | Op::MatrixTimesMatrix => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::Multiply, BINARY)?;
                }
                Op::Transpose => {
                    inst.expect(4)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let matrix_id = self.next()?;
                    let matrix_lexp = self.lookup_expression.lookup(matrix_id)?;
                    let matrix_handle = get_expr_handle!(matrix_id, matrix_lexp);
                    let expr = crate::Expression::Math {
                        fun: crate::MathFunction::Transpose,
                        arg: matrix_handle,
                        arg1: None,
                        arg2: None,
                        arg3: None,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::Dot => {
                    inst.expect(5)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let left_id = self.next()?;
                    let right_id = self.next()?;
                    let left_lexp = self.lookup_expression.lookup(left_id)?;
                    let left_handle = get_expr_handle!(left_id, left_lexp);
                    let right_lexp = self.lookup_expression.lookup(right_id)?;
                    let right_handle = get_expr_handle!(right_id, right_lexp);
                    let expr = crate::Expression::Math {
                        fun: crate::MathFunction::Dot,
                        arg: left_handle,
                        arg1: Some(right_handle),
                        arg2: None,
                        arg3: None,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::BitFieldInsert => {
                    inst.expect(7)?;

                    let start = self.data_offset;
                    let span = self.span_from_with_op(start);

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let base_id = self.next()?;
                    let insert_id = self.next()?;
                    let offset_id = self.next()?;
                    let count_id = self.next()?;
                    let base_lexp = self.lookup_expression.lookup(base_id)?;
                    let base_handle = get_expr_handle!(base_id, base_lexp);
                    let insert_lexp = self.lookup_expression.lookup(insert_id)?;
                    let insert_handle = get_expr_handle!(insert_id, insert_lexp);
                    let offset_lexp = self.lookup_expression.lookup(offset_id)?;
                    let offset_handle = get_expr_handle!(offset_id, offset_lexp);
                    let offset_lookup_ty = self.lookup_type.lookup(offset_lexp.type_id)?;
                    let count_lexp = self.lookup_expression.lookup(count_id)?;
                    let count_handle = get_expr_handle!(count_id, count_lexp);
                    let count_lookup_ty = self.lookup_type.lookup(count_lexp.type_id)?;

                    let offset_kind = ctx.module.types[offset_lookup_ty.handle]
                        .inner
                        .scalar_kind()
                        .unwrap();
                    let count_kind = ctx.module.types[count_lookup_ty.handle]
                        .inner
                        .scalar_kind()
                        .unwrap();

                    let offset_cast_handle = if offset_kind != crate::ScalarKind::Uint {
                        ctx.expressions.append(
                            crate::Expression::As {
                                expr: offset_handle,
                                kind: crate::ScalarKind::Uint,
                                convert: None,
                            },
                            span,
                        )
                    } else {
                        offset_handle
                    };

                    let count_cast_handle = if count_kind != crate::ScalarKind::Uint {
                        ctx.expressions.append(
                            crate::Expression::As {
                                expr: count_handle,
                                kind: crate::ScalarKind::Uint,
                                convert: None,
                            },
                            span,
                        )
                    } else {
                        count_handle
                    };

                    let expr = crate::Expression::Math {
                        fun: crate::MathFunction::InsertBits,
                        arg: base_handle,
                        arg1: Some(insert_handle),
                        arg2: Some(offset_cast_handle),
                        arg3: Some(count_cast_handle),
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::BitFieldSExtract | Op::BitFieldUExtract => {
                    inst.expect(6)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let base_id = self.next()?;
                    let offset_id = self.next()?;
                    let count_id = self.next()?;
                    let base_lexp = self.lookup_expression.lookup(base_id)?;
                    let base_handle = get_expr_handle!(base_id, base_lexp);
                    let offset_lexp = self.lookup_expression.lookup(offset_id)?;
                    let offset_handle = get_expr_handle!(offset_id, offset_lexp);
                    let offset_lookup_ty = self.lookup_type.lookup(offset_lexp.type_id)?;
                    let count_lexp = self.lookup_expression.lookup(count_id)?;
                    let count_handle = get_expr_handle!(count_id, count_lexp);
                    let count_lookup_ty = self.lookup_type.lookup(count_lexp.type_id)?;

                    let offset_kind = ctx.module.types[offset_lookup_ty.handle]
                        .inner
                        .scalar_kind()
                        .unwrap();
                    let count_kind = ctx.module.types[count_lookup_ty.handle]
                        .inner
                        .scalar_kind()
                        .unwrap();

                    let offset_cast_handle = if offset_kind != crate::ScalarKind::Uint {
                        ctx.expressions.append(
                            crate::Expression::As {
                                expr: offset_handle,
                                kind: crate::ScalarKind::Uint,
                                convert: None,
                            },
                            span,
                        )
                    } else {
                        offset_handle
                    };

                    let count_cast_handle = if count_kind != crate::ScalarKind::Uint {
                        ctx.expressions.append(
                            crate::Expression::As {
                                expr: count_handle,
                                kind: crate::ScalarKind::Uint,
                                convert: None,
                            },
                            span,
                        )
                    } else {
                        count_handle
                    };

                    let expr = crate::Expression::Math {
                        fun: crate::MathFunction::ExtractBits,
                        arg: base_handle,
                        arg1: Some(offset_cast_handle),
                        arg2: Some(count_cast_handle),
                        arg3: None,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::BitReverse | Op::BitCount => {
                    inst.expect(4)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let base_id = self.next()?;
                    let base_lexp = self.lookup_expression.lookup(base_id)?;
                    let base_handle = get_expr_handle!(base_id, base_lexp);
                    let expr = crate::Expression::Math {
                        fun: match inst.op {
                            Op::BitReverse => crate::MathFunction::ReverseBits,
                            Op::BitCount => crate::MathFunction::CountOneBits,
                            _ => unreachable!(),
                        },
                        arg: base_handle,
                        arg1: None,
                        arg2: None,
                        arg3: None,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::OuterProduct => {
                    inst.expect(5)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let left_id = self.next()?;
                    let right_id = self.next()?;
                    let left_lexp = self.lookup_expression.lookup(left_id)?;
                    let left_handle = get_expr_handle!(left_id, left_lexp);
                    let right_lexp = self.lookup_expression.lookup(right_id)?;
                    let right_handle = get_expr_handle!(right_id, right_lexp);
                    let expr = crate::Expression::Math {
                        fun: crate::MathFunction::Outer,
                        arg: left_handle,
                        arg1: Some(right_handle),
                        arg2: None,
                        arg3: None,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                // Bitwise instructions
                Op::Not => {
                    inst.expect(4)?;
                    self.parse_expr_unary_op_sign_adjusted(
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                        crate::UnaryOperator::BitwiseNot,
                    )?;
                }
                Op::ShiftRightLogical => {
                    inst.expect(5)?;
                    //TODO: convert input and result to unsigned
                    parse_expr_op!(crate::BinaryOperator::ShiftRight, SHIFT)?;
                }
                Op::ShiftRightArithmetic => {
                    inst.expect(5)?;
                    //TODO: convert input and result to signed
                    parse_expr_op!(crate::BinaryOperator::ShiftRight, SHIFT)?;
                }
                Op::ShiftLeftLogical => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::ShiftLeft, SHIFT)?;
                }
                // Sampling
                Op::Image => {
                    inst.expect(4)?;
                    self.parse_image_uncouple(block_id)?;
                }
                Op::SampledImage => {
                    inst.expect(5)?;
                    self.parse_image_couple()?;
                }
                Op::ImageWrite => {
                    let extra = inst.expect_at_least(4)?;
                    let stmt =
                        self.parse_image_write(extra, ctx, &mut emitter, &mut block, body_idx)?;
                    block.extend(emitter.finish(ctx.expressions));
                    block.push(stmt, span);
                    emitter.start(ctx.expressions);
                }
                Op::ImageFetch | Op::ImageRead => {
                    let extra = inst.expect_at_least(5)?;
                    self.parse_image_load(
                        extra,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageSampleImplicitLod | Op::ImageSampleExplicitLod => {
                    let extra = inst.expect_at_least(5)?;
                    let options = image::SamplingOptions {
                        compare: false,
                        project: false,
                    };
                    self.parse_image_sample(
                        extra,
                        options,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageSampleProjImplicitLod | Op::ImageSampleProjExplicitLod => {
                    let extra = inst.expect_at_least(5)?;
                    let options = image::SamplingOptions {
                        compare: false,
                        project: true,
                    };
                    self.parse_image_sample(
                        extra,
                        options,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageSampleDrefImplicitLod | Op::ImageSampleDrefExplicitLod => {
                    let extra = inst.expect_at_least(6)?;
                    let options = image::SamplingOptions {
                        compare: true,
                        project: false,
                    };
                    self.parse_image_sample(
                        extra,
                        options,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageSampleProjDrefImplicitLod | Op::ImageSampleProjDrefExplicitLod => {
                    let extra = inst.expect_at_least(6)?;
                    let options = image::SamplingOptions {
                        compare: true,
                        project: true,
                    };
                    self.parse_image_sample(
                        extra,
                        options,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageQuerySize => {
                    inst.expect(4)?;
                    self.parse_image_query_size(
                        false,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageQuerySizeLod => {
                    inst.expect(5)?;
                    self.parse_image_query_size(
                        true,
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                    )?;
                }
                Op::ImageQueryLevels => {
                    inst.expect(4)?;
                    self.parse_image_query_other(crate::ImageQuery::NumLevels, ctx, block_id)?;
                }
                Op::ImageQuerySamples => {
                    inst.expect(4)?;
                    self.parse_image_query_other(crate::ImageQuery::NumSamples, ctx, block_id)?;
                }
                // other ops
                Op::Select => {
                    inst.expect(6)?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let condition = self.next()?;
                    let o1_id = self.next()?;
                    let o2_id = self.next()?;

                    let cond_lexp = self.lookup_expression.lookup(condition)?;
                    let cond_handle = get_expr_handle!(condition, cond_lexp);
                    let o1_lexp = self.lookup_expression.lookup(o1_id)?;
                    let o1_handle = get_expr_handle!(o1_id, o1_lexp);
                    let o2_lexp = self.lookup_expression.lookup(o2_id)?;
                    let o2_handle = get_expr_handle!(o2_id, o2_lexp);

                    let expr = crate::Expression::Select {
                        condition: cond_handle,
                        accept: o1_handle,
                        reject: o2_handle,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::VectorShuffle => {
                    inst.expect_at_least(5)?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let v1_id = self.next()?;
                    let v2_id = self.next()?;

                    let v1_lexp = self.lookup_expression.lookup(v1_id)?;
                    let v1_lty = self.lookup_type.lookup(v1_lexp.type_id)?;
                    let v1_handle = get_expr_handle!(v1_id, v1_lexp);
                    let n1 = match ctx.module.types[v1_lty.handle].inner {
                        crate::TypeInner::Vector { size, .. } => size as u32,
                        _ => return Err(Error::InvalidInnerType(v1_lexp.type_id)),
                    };
                    let v2_lexp = self.lookup_expression.lookup(v2_id)?;
                    let v2_lty = self.lookup_type.lookup(v2_lexp.type_id)?;
                    let v2_handle = get_expr_handle!(v2_id, v2_lexp);
                    let n2 = match ctx.module.types[v2_lty.handle].inner {
                        crate::TypeInner::Vector { size, .. } => size as u32,
                        _ => return Err(Error::InvalidInnerType(v2_lexp.type_id)),
                    };

                    self.temp_bytes.clear();
                    let mut max_component = 0;
                    for _ in 5..inst.wc as usize {
                        let mut index = self.next()?;
                        if index == u32::MAX {
                            // treat Undefined as X
                            index = 0;
                        }
                        max_component = max_component.max(index);
                        self.temp_bytes.push(index as u8);
                    }

                    // Check for swizzle first.
                    let expr = if max_component < n1 {
                        use crate::SwizzleComponent as Sc;
                        let size = match self.temp_bytes.len() {
                            2 => crate::VectorSize::Bi,
                            3 => crate::VectorSize::Tri,
                            _ => crate::VectorSize::Quad,
                        };
                        let mut pattern = [Sc::X; 4];
                        for (pat, index) in pattern.iter_mut().zip(self.temp_bytes.drain(..)) {
                            *pat = match index {
                                0 => Sc::X,
                                1 => Sc::Y,
                                2 => Sc::Z,
                                _ => Sc::W,
                            };
                        }
                        crate::Expression::Swizzle {
                            size,
                            vector: v1_handle,
                            pattern,
                        }
                    } else {
                        // Fall back to access + compose
                        let mut components = Vec::with_capacity(self.temp_bytes.len());
                        for index in self.temp_bytes.drain(..).map(|i| i as u32) {
                            let expr = if index < n1 {
                                crate::Expression::AccessIndex {
                                    base: v1_handle,
                                    index,
                                }
                            } else if index < n1 + n2 {
                                crate::Expression::AccessIndex {
                                    base: v2_handle,
                                    index: index - n1,
                                }
                            } else {
                                return Err(Error::InvalidAccessIndex(index));
                            };
                            components.push(ctx.expressions.append(expr, span));
                        }
                        crate::Expression::Compose {
                            ty: self.lookup_type.lookup(result_type_id)?.handle,
                            components,
                        }
                    };

                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::Bitcast
                | Op::ConvertSToF
                | Op::ConvertUToF
                | Op::ConvertFToU
                | Op::ConvertFToS
                | Op::FConvert
                | Op::UConvert
                | Op::SConvert => {
                    inst.expect(4)?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let value_id = self.next()?;

                    let value_lexp = self.lookup_expression.lookup(value_id)?;
                    let ty_lookup = self.lookup_type.lookup(result_type_id)?;
                    let scalar = match ctx.module.types[ty_lookup.handle].inner {
                        crate::TypeInner::Scalar(scalar)
                        | crate::TypeInner::Vector { scalar, .. }
                        | crate::TypeInner::Matrix { scalar, .. } => scalar,
                        _ => return Err(Error::InvalidAsType(ty_lookup.handle)),
                    };

                    let expr = crate::Expression::As {
                        expr: get_expr_handle!(value_id, value_lexp),
                        kind: scalar.kind,
                        convert: if scalar.kind == crate::ScalarKind::Bool {
                            Some(crate::BOOL_WIDTH)
                        } else if inst.op == Op::Bitcast {
                            None
                        } else {
                            Some(scalar.width)
                        },
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::FunctionCall => {
                    inst.expect_at_least(4)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let func_id = self.next()?;

                    let mut arguments = Vec::with_capacity(inst.wc as usize - 4);
                    for _ in 0..arguments.capacity() {
                        let arg_id = self.next()?;
                        let lexp = self.lookup_expression.lookup(arg_id)?;
                        arguments.push(get_expr_handle!(arg_id, lexp));
                    }

                    block.extend(emitter.finish(ctx.expressions));

                    // We just need an unique handle here, nothing more.
                    let function = self.add_call(ctx.function_id, func_id);

                    let result = if self.lookup_void_type == Some(result_type_id) {
                        None
                    } else {
                        let expr_handle = ctx
                            .expressions
                            .append(crate::Expression::CallResult(function), span);
                        self.lookup_expression.insert(
                            result_id,
                            LookupExpression {
                                handle: expr_handle,
                                type_id: result_type_id,
                                block_id,
                            },
                        );
                        Some(expr_handle)
                    };
                    block.push(
                        crate::Statement::Call {
                            function,
                            arguments,
                            result,
                        },
                        span,
                    );
                    emitter.start(ctx.expressions);
                }
                Op::ExtInst => {
                    use crate::MathFunction as Mf;
                    use spirv::GLOp as Glo;

                    let base_wc = 5;
                    inst.expect_at_least(base_wc)?;

                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let set_id = self.next()?;
                    if Some(set_id) != self.ext_glsl_id {
                        return Err(Error::UnsupportedExtInstSet(set_id));
                    }
                    let inst_id = self.next()?;
                    let gl_op = Glo::from_u32(inst_id).ok_or(Error::UnsupportedExtInst(inst_id))?;

                    let fun = match gl_op {
                        Glo::Round => Mf::Round,
                        Glo::RoundEven => Mf::Round,
                        Glo::Trunc => Mf::Trunc,
                        Glo::FAbs | Glo::SAbs => Mf::Abs,
                        Glo::FSign | Glo::SSign => Mf::Sign,
                        Glo::Floor => Mf::Floor,
                        Glo::Ceil => Mf::Ceil,
                        Glo::Fract => Mf::Fract,
                        Glo::Sin => Mf::Sin,
                        Glo::Cos => Mf::Cos,
                        Glo::Tan => Mf::Tan,
                        Glo::Asin => Mf::Asin,
                        Glo::Acos => Mf::Acos,
                        Glo::Atan => Mf::Atan,
                        Glo::Sinh => Mf::Sinh,
                        Glo::Cosh => Mf::Cosh,
                        Glo::Tanh => Mf::Tanh,
                        Glo::Atan2 => Mf::Atan2,
                        Glo::Asinh => Mf::Asinh,
                        Glo::Acosh => Mf::Acosh,
                        Glo::Atanh => Mf::Atanh,
                        Glo::Radians => Mf::Radians,
                        Glo::Degrees => Mf::Degrees,
                        Glo::Pow => Mf::Pow,
                        Glo::Exp => Mf::Exp,
                        Glo::Log => Mf::Log,
                        Glo::Exp2 => Mf::Exp2,
                        Glo::Log2 => Mf::Log2,
                        Glo::Sqrt => Mf::Sqrt,
                        Glo::InverseSqrt => Mf::InverseSqrt,
                        Glo::MatrixInverse => Mf::Inverse,
                        Glo::Determinant => Mf::Determinant,
                        Glo::ModfStruct => Mf::Modf,
                        Glo::FMin | Glo::UMin | Glo::SMin | Glo::NMin => Mf::Min,
                        Glo::FMax | Glo::UMax | Glo::SMax | Glo::NMax => Mf::Max,
                        Glo::FClamp | Glo::UClamp | Glo::SClamp | Glo::NClamp => Mf::Clamp,
                        Glo::FMix => Mf::Mix,
                        Glo::Step => Mf::Step,
                        Glo::SmoothStep => Mf::SmoothStep,
                        Glo::Fma => Mf::Fma,
                        Glo::FrexpStruct => Mf::Frexp,
                        Glo::Ldexp => Mf::Ldexp,
                        Glo::Length => Mf::Length,
                        Glo::Distance => Mf::Distance,
                        Glo::Cross => Mf::Cross,
                        Glo::Normalize => Mf::Normalize,
                        Glo::FaceForward => Mf::FaceForward,
                        Glo::Reflect => Mf::Reflect,
                        Glo::Refract => Mf::Refract,
                        Glo::PackUnorm4x8 => Mf::Pack4x8unorm,
                        Glo::PackSnorm4x8 => Mf::Pack4x8snorm,
                        Glo::PackHalf2x16 => Mf::Pack2x16float,
                        Glo::PackUnorm2x16 => Mf::Pack2x16unorm,
                        Glo::PackSnorm2x16 => Mf::Pack2x16snorm,
                        Glo::UnpackUnorm4x8 => Mf::Unpack4x8unorm,
                        Glo::UnpackSnorm4x8 => Mf::Unpack4x8snorm,
                        Glo::UnpackHalf2x16 => Mf::Unpack2x16float,
                        Glo::UnpackUnorm2x16 => Mf::Unpack2x16unorm,
                        Glo::UnpackSnorm2x16 => Mf::Unpack2x16snorm,
                        Glo::FindILsb => Mf::FirstTrailingBit,
                        Glo::FindUMsb | Glo::FindSMsb => Mf::FirstLeadingBit,
                        // TODO: https://github.com/gfx-rs/naga/issues/2526
                        Glo::Modf | Glo::Frexp => return Err(Error::UnsupportedExtInst(inst_id)),
                        Glo::IMix
                        | Glo::PackDouble2x32
                        | Glo::UnpackDouble2x32
                        | Glo::InterpolateAtCentroid
                        | Glo::InterpolateAtSample
                        | Glo::InterpolateAtOffset => {
                            return Err(Error::UnsupportedExtInst(inst_id))
                        }
                    };

                    let arg_count = fun.argument_count();
                    inst.expect(base_wc + arg_count as u16)?;
                    let arg = {
                        let arg_id = self.next()?;
                        let lexp = self.lookup_expression.lookup(arg_id)?;
                        get_expr_handle!(arg_id, lexp)
                    };
                    let arg1 = if arg_count > 1 {
                        let arg_id = self.next()?;
                        let lexp = self.lookup_expression.lookup(arg_id)?;
                        Some(get_expr_handle!(arg_id, lexp))
                    } else {
                        None
                    };
                    let arg2 = if arg_count > 2 {
                        let arg_id = self.next()?;
                        let lexp = self.lookup_expression.lookup(arg_id)?;
                        Some(get_expr_handle!(arg_id, lexp))
                    } else {
                        None
                    };
                    let arg3 = if arg_count > 3 {
                        let arg_id = self.next()?;
                        let lexp = self.lookup_expression.lookup(arg_id)?;
                        Some(get_expr_handle!(arg_id, lexp))
                    } else {
                        None
                    };

                    let expr = crate::Expression::Math {
                        fun,
                        arg,
                        arg1,
                        arg2,
                        arg3,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                // Relational and Logical Instructions
                Op::LogicalNot => {
                    inst.expect(4)?;
                    parse_expr_op!(crate::UnaryOperator::LogicalNot, UNARY)?;
                }
                Op::LogicalOr => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::LogicalOr, BINARY)?;
                }
                Op::LogicalAnd => {
                    inst.expect(5)?;
                    parse_expr_op!(crate::BinaryOperator::LogicalAnd, BINARY)?;
                }
                Op::SGreaterThan | Op::SGreaterThanEqual | Op::SLessThan | Op::SLessThanEqual => {
                    inst.expect(5)?;
                    self.parse_expr_int_comparison(
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                        map_binary_operator(inst.op)?,
                        crate::ScalarKind::Sint,
                    )?;
                }
                Op::UGreaterThan | Op::UGreaterThanEqual | Op::ULessThan | Op::ULessThanEqual => {
                    inst.expect(5)?;
                    self.parse_expr_int_comparison(
                        ctx,
                        &mut emitter,
                        &mut block,
                        block_id,
                        body_idx,
                        map_binary_operator(inst.op)?,
                        crate::ScalarKind::Uint,
                    )?;
                }
                Op::FOrdEqual
                | Op::FUnordEqual
                | Op::FOrdNotEqual
                | Op::FUnordNotEqual
                | Op::FOrdLessThan
                | Op::FUnordLessThan
                | Op::FOrdGreaterThan
                | Op::FUnordGreaterThan
                | Op::FOrdLessThanEqual
                | Op::FUnordLessThanEqual
                | Op::FOrdGreaterThanEqual
                | Op::FUnordGreaterThanEqual
                | Op::LogicalEqual
                | Op::LogicalNotEqual => {
                    inst.expect(5)?;
                    let operator = map_binary_operator(inst.op)?;
                    parse_expr_op!(operator, BINARY)?;
                }
                Op::Any | Op::All | Op::IsNan | Op::IsInf | Op::IsFinite | Op::IsNormal => {
                    inst.expect(4)?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let arg_id = self.next()?;

                    let arg_lexp = self.lookup_expression.lookup(arg_id)?;
                    let arg_handle = get_expr_handle!(arg_id, arg_lexp);

                    let expr = crate::Expression::Relational {
                        fun: map_relational_fun(inst.op)?,
                        argument: arg_handle,
                    };
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: ctx.expressions.append(expr, span),
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::Kill => {
                    inst.expect(1)?;
                    break Some(crate::Statement::Kill);
                }
                Op::Unreachable => {
                    inst.expect(1)?;
                    break None;
                }
                Op::Return => {
                    inst.expect(1)?;
                    break Some(crate::Statement::Return { value: None });
                }
                Op::ReturnValue => {
                    inst.expect(2)?;
                    let value_id = self.next()?;
                    let value_lexp = self.lookup_expression.lookup(value_id)?;
                    let value_handle = get_expr_handle!(value_id, value_lexp);
                    break Some(crate::Statement::Return {
                        value: Some(value_handle),
                    });
                }
                Op::Branch => {
                    inst.expect(2)?;
                    let target_id = self.next()?;

                    // If this is a branch to a merge or continue block, then
                    // that ends the current body.
                    //
                    // Why can we count on finding an entry here when it's
                    // needed? SPIR-V requires dominators to appear before
                    // blocks they dominate, so we will have visited a
                    // structured control construct's header block before
                    // anything that could exit it.
                    if let Some(info) = ctx.mergers.get(&target_id) {
                        block.extend(emitter.finish(ctx.expressions));
                        ctx.blocks.insert(block_id, block);
                        let body = &mut ctx.bodies[body_idx];
                        body.data.push(BodyFragment::BlockId(block_id));

                        merger(body, info);

                        return Ok(());
                    }

                    // If `target_id` has no entry in `ctx.body_for_label`, then
                    // this must be the only branch to it:
                    //
                    // - We've already established that it's not anybody's merge
                    //   block.
                    //
                    // - It can't be a switch case. Only switch header blocks
                    //   and other switch cases can branch to a switch case.
                    //   Switch header blocks must dominate all their cases, so
                    //   they must appear in the file before them, and when we
                    //   see `Op::Switch` we populate `ctx.body_for_label` for
                    //   every switch case.
                    //
                    // Thus, `target_id` must be a simple extension of the
                    // current block, which we dominate, so we know we'll
                    // encounter it later in the file.
                    ctx.body_for_label.entry(target_id).or_insert(body_idx);

                    break None;
                }
                Op::BranchConditional => {
                    inst.expect_at_least(4)?;

                    let condition = {
                        let condition_id = self.next()?;
                        let lexp = self.lookup_expression.lookup(condition_id)?;
                        get_expr_handle!(condition_id, lexp)
                    };

                    // HACK(eddyb) Naga doesn't seem to have this helper,
                    // so it's declared on the fly here for convenience.
                    #[derive(Copy, Clone)]
                    struct BranchTarget {
                        label_id: spirv::Word,
                        merge_info: Option<MergeBlockInformation>,
                    }
                    let branch_target = |label_id| BranchTarget {
                        label_id,
                        merge_info: ctx.mergers.get(&label_id).copied(),
                    };

                    let true_target = branch_target(self.next()?);
                    let false_target = branch_target(self.next()?);

                    // Consume branch weights
                    for _ in 4..inst.wc {
                        let _ = self.next()?;
                    }

                    // Handle `OpBranchConditional`s used at the end of a loop
                    // body's "continuing" section as a "conditional backedge",
                    // i.e. a `do`-`while` condition, or `break if` in WGSL.

                    // HACK(eddyb) this has to go to the parent *twice*, because
                    // `OpLoopMerge` left the "continuing" section nested in the
                    // loop body in terms of `parent`, but not `BodyFragment`.
                    let parent_body_idx = ctx.bodies[body_idx].parent;
                    let parent_parent_body_idx = ctx.bodies[parent_body_idx].parent;
                    match ctx.bodies[parent_parent_body_idx].data[..] {
                        // The `OpLoopMerge`'s `continuing` block and the loop's
                        // backedge block may not be the same, but they'll both
                        // belong to the same body.
                        [.., BodyFragment::Loop {
                            body: loop_body_idx,
                            continuing: loop_continuing_idx,
                            break_if: ref mut break_if_slot @ None,
                        }] if body_idx == loop_continuing_idx => {
                            // Try both orderings of break-vs-backedge, because
                            // SPIR-V is symmetrical here, unlike WGSL `break if`.
                            let break_if_cond = [true, false].into_iter().find_map(|true_breaks| {
                                let (break_candidate, backedge_candidate) = if true_breaks {
                                    (true_target, false_target)
                                } else {
                                    (false_target, true_target)
                                };

                                if break_candidate.merge_info
                                    != Some(MergeBlockInformation::LoopMerge)
                                {
                                    return None;
                                }

                                // HACK(eddyb) since Naga doesn't explicitly track
                                // backedges, this is checking for the outcome of
                                // `OpLoopMerge` below (even if it looks weird).
                                let backedge_candidate_is_backedge =
                                    backedge_candidate.merge_info.is_none()
                                        && ctx.body_for_label.get(&backedge_candidate.label_id)
                                            == Some(&loop_body_idx);
                                if !backedge_candidate_is_backedge {
                                    return None;
                                }

                                Some(if true_breaks {
                                    condition
                                } else {
                                    ctx.expressions.append(
                                        crate::Expression::Unary {
                                            op: crate::UnaryOperator::LogicalNot,
                                            expr: condition,
                                        },
                                        span,
                                    )
                                })
                            });

                            if let Some(break_if_cond) = break_if_cond {
                                *break_if_slot = Some(break_if_cond);

                                // This `OpBranchConditional` ends the "continuing"
                                // section of the loop body as normal, with the
                                // `break if` condition having been stashed above.
                                break None;
                            }
                        }
                        _ => {}
                    }

                    block.extend(emitter.finish(ctx.expressions));
                    ctx.blocks.insert(block_id, block);
                    let body = &mut ctx.bodies[body_idx];
                    body.data.push(BodyFragment::BlockId(block_id));

                    let same_target = true_target.label_id == false_target.label_id;

                    // Start a body block for the `accept` branch.
                    let accept = ctx.bodies.len();
                    let mut accept_block = Body::with_parent(body_idx);

                    // If the `OpBranchConditional` target is somebody else's
                    // merge or continue block, then put a `Break` or `Continue`
                    // statement in this new body block.
                    if let Some(info) = true_target.merge_info {
                        merger(
                            match same_target {
                                true => &mut ctx.bodies[body_idx],
                                false => &mut accept_block,
                            },
                            &info,
                        )
                    } else {
                        // Note the body index for the block we're branching to.
                        let prev = ctx.body_for_label.insert(
                            true_target.label_id,
                            match same_target {
                                true => body_idx,
                                false => accept,
                            },
                        );
                        debug_assert!(prev.is_none());
                    }

                    if same_target {
                        return Ok(());
                    }

                    ctx.bodies.push(accept_block);

                    // Handle the `reject` branch just like the `accept` block.
                    let reject = ctx.bodies.len();
                    let mut reject_block = Body::with_parent(body_idx);

                    if let Some(info) = false_target.merge_info {
                        merger(&mut reject_block, &info)
                    } else {
                        let prev = ctx.body_for_label.insert(false_target.label_id, reject);
                        debug_assert!(prev.is_none());
                    }

                    ctx.bodies.push(reject_block);

                    let body = &mut ctx.bodies[body_idx];
                    body.data.push(BodyFragment::If {
                        condition,
                        accept,
                        reject,
                    });

                    return Ok(());
                }
                Op::Switch => {
                    inst.expect_at_least(3)?;
                    let selector = self.next()?;
                    let default_id = self.next()?;

                    // If the previous instruction was a `OpSelectionMerge` then we must
                    // promote the `MergeBlockInformation` to a `SwitchMerge`
                    if let Some(merge) = selection_merge_block {
                        ctx.mergers
                            .insert(merge, MergeBlockInformation::SwitchMerge);
                    }

                    let default = ctx.bodies.len();
                    ctx.bodies.push(Body::with_parent(body_idx));
                    ctx.body_for_label.entry(default_id).or_insert(default);

                    let selector_lexp = &self.lookup_expression[&selector];
                    let selector_lty = self.lookup_type.lookup(selector_lexp.type_id)?;
                    let selector_handle = get_expr_handle!(selector, selector_lexp);
                    let selector = match ctx.module.types[selector_lty.handle].inner {
                        crate::TypeInner::Scalar(crate::Scalar {
                            kind: crate::ScalarKind::Uint,
                            width: _,
                        }) => {
                            // IR expects a signed integer, so do a bitcast
                            ctx.expressions.append(
                                crate::Expression::As {
                                    kind: crate::ScalarKind::Sint,
                                    expr: selector_handle,
                                    convert: None,
                                },
                                span,
                            )
                        }
                        crate::TypeInner::Scalar(crate::Scalar {
                            kind: crate::ScalarKind::Sint,
                            width: _,
                        }) => selector_handle,
                        ref other => unimplemented!("Unexpected selector {:?}", other),
                    };

                    // Clear past switch cases to prevent them from entering this one
                    self.switch_cases.clear();

                    for _ in 0..(inst.wc - 3) / 2 {
                        let literal = self.next()?;
                        let target = self.next()?;

                        let case_body_idx = ctx.bodies.len();

                        // Check if any previous case already used this target block id, if so
                        // group them together to reorder them later so that no weird
                        // fallthrough cases happen.
                        if let Some(&mut (_, ref mut literals)) = self.switch_cases.get_mut(&target)
                        {
                            literals.push(literal as i32);
                            continue;
                        }

                        let mut body = Body::with_parent(body_idx);

                        if let Some(info) = ctx.mergers.get(&target) {
                            merger(&mut body, info);
                        }

                        ctx.bodies.push(body);
                        ctx.body_for_label.entry(target).or_insert(case_body_idx);

                        // Register this target block id as already having been processed and
                        // the respective body index assigned and the first case value
                        self.switch_cases
                            .insert(target, (case_body_idx, vec![literal as i32]));
                    }

                    // Loop through the collected target blocks creating a new case for each
                    // literal pointing to it, only one case will have the true body and all the
                    // others will be empty fallthrough so that they all execute the same body
                    // without duplicating code.
                    //
                    // Since `switch_cases` is an indexmap the order of insertion is preserved
                    // this is needed because spir-v defines fallthrough order in the switch
                    // instruction.
                    let mut cases = Vec::with_capacity((inst.wc as usize - 3) / 2);
                    for &(case_body_idx, ref literals) in self.switch_cases.values() {
                        let value = literals[0];

                        for &literal in literals.iter().skip(1) {
                            let empty_body_idx = ctx.bodies.len();
                            let body = Body::with_parent(body_idx);

                            ctx.bodies.push(body);

                            cases.push((literal, empty_body_idx));
                        }

                        cases.push((value, case_body_idx));
                    }

                    block.extend(emitter.finish(ctx.expressions));

                    let body = &mut ctx.bodies[body_idx];
                    ctx.blocks.insert(block_id, block);
                    // Make sure the vector has space for at least two more allocations
                    body.data.reserve(2);
                    body.data.push(BodyFragment::BlockId(block_id));
                    body.data.push(BodyFragment::Switch {
                        selector,
                        cases,
                        default,
                    });

                    return Ok(());
                }
                Op::SelectionMerge => {
                    inst.expect(3)?;
                    let merge_block_id = self.next()?;
                    // TODO: Selection Control Mask
                    let _selection_control = self.next()?;

                    // Indicate that the merge block is a continuation of the
                    // current `Body`.
                    ctx.body_for_label.entry(merge_block_id).or_insert(body_idx);

                    // Let subsequent branches to the merge block know that
                    // they've reached the end of the selection construct.
                    ctx.mergers
                        .insert(merge_block_id, MergeBlockInformation::SelectionMerge);

                    selection_merge_block = Some(merge_block_id);
                }
                Op::LoopMerge => {
                    inst.expect_at_least(4)?;
                    let merge_block_id = self.next()?;
                    let continuing = self.next()?;

                    // TODO: Loop Control Parameters
                    for _ in 0..inst.wc - 3 {
                        self.next()?;
                    }

                    // Indicate that the merge block is a continuation of the
                    // current `Body`.
                    ctx.body_for_label.entry(merge_block_id).or_insert(body_idx);
                    // Let subsequent branches to the merge block know that
                    // they're `Break` statements.
                    ctx.mergers
                        .insert(merge_block_id, MergeBlockInformation::LoopMerge);

                    let loop_body_idx = ctx.bodies.len();
                    ctx.bodies.push(Body::with_parent(body_idx));

                    let continue_idx = ctx.bodies.len();
                    // The continue block inherits the scope of the loop body
                    ctx.bodies.push(Body::with_parent(loop_body_idx));
                    ctx.body_for_label.entry(continuing).or_insert(continue_idx);
                    // Let subsequent branches to the continue block know that
                    // they're `Continue` statements.
                    ctx.mergers
                        .insert(continuing, MergeBlockInformation::LoopContinue);

                    // The loop header always belongs to the loop body
                    ctx.body_for_label.insert(block_id, loop_body_idx);

                    let parent_body = &mut ctx.bodies[body_idx];
                    parent_body.data.push(BodyFragment::Loop {
                        body: loop_body_idx,
                        continuing: continue_idx,
                        break_if: None,
                    });
                    body_idx = loop_body_idx;
                }
                Op::DPdxCoarse => {
                    parse_expr_op!(
                        crate::DerivativeAxis::X,
                        crate::DerivativeControl::Coarse,
                        DERIVATIVE
                    )?;
                }
                Op::DPdyCoarse => {
                    parse_expr_op!(
                        crate::DerivativeAxis::Y,
                        crate::DerivativeControl::Coarse,
                        DERIVATIVE
                    )?;
                }
                Op::FwidthCoarse => {
                    parse_expr_op!(
                        crate::DerivativeAxis::Width,
                        crate::DerivativeControl::Coarse,
                        DERIVATIVE
                    )?;
                }
                Op::DPdxFine => {
                    parse_expr_op!(
                        crate::DerivativeAxis::X,
                        crate::DerivativeControl::Fine,
                        DERIVATIVE
                    )?;
                }
                Op::DPdyFine => {
                    parse_expr_op!(
                        crate::DerivativeAxis::Y,
                        crate::DerivativeControl::Fine,
                        DERIVATIVE
                    )?;
                }
                Op::FwidthFine => {
                    parse_expr_op!(
                        crate::DerivativeAxis::Width,
                        crate::DerivativeControl::Fine,
                        DERIVATIVE
                    )?;
                }
                Op::DPdx => {
                    parse_expr_op!(
                        crate::DerivativeAxis::X,
                        crate::DerivativeControl::None,
                        DERIVATIVE
                    )?;
                }
                Op::DPdy => {
                    parse_expr_op!(
                        crate::DerivativeAxis::Y,
                        crate::DerivativeControl::None,
                        DERIVATIVE
                    )?;
                }
                Op::Fwidth => {
                    parse_expr_op!(
                        crate::DerivativeAxis::Width,
                        crate::DerivativeControl::None,
                        DERIVATIVE
                    )?;
                }
                Op::ArrayLength => {
                    inst.expect(5)?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let structure_id = self.next()?;
                    let member_index = self.next()?;

                    // We're assuming that the validation pass, if it's run, will catch if the
                    // wrong types or parameters are supplied here.

                    let structure_ptr = self.lookup_expression.lookup(structure_id)?;
                    let structure_handle = get_expr_handle!(structure_id, structure_ptr);

                    let member_ptr = ctx.expressions.append(
                        crate::Expression::AccessIndex {
                            base: structure_handle,
                            index: member_index,
                        },
                        span,
                    );

                    let length = ctx
                        .expressions
                        .append(crate::Expression::ArrayLength(member_ptr), span);

                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: length,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::CopyMemory => {
                    inst.expect_at_least(3)?;
                    let target_id = self.next()?;
                    let source_id = self.next()?;
                    let _memory_access = if inst.wc != 3 {
                        inst.expect(4)?;
                        spirv::MemoryAccess::from_bits(self.next()?)
                            .ok_or(Error::InvalidParameter(Op::CopyMemory))?
                    } else {
                        spirv::MemoryAccess::NONE
                    };

                    // TODO: check if the source and target types are the same?
                    let target = self.lookup_expression.lookup(target_id)?;
                    let target_handle = get_expr_handle!(target_id, target);
                    let source = self.lookup_expression.lookup(source_id)?;
                    let source_handle = get_expr_handle!(source_id, source);

                    // This operation is practically the same as loading and then storing, I think.
                    let value_expr = ctx.expressions.append(
                        crate::Expression::Load {
                            pointer: source_handle,
                        },
                        span,
                    );

                    block.extend(emitter.finish(ctx.expressions));
                    block.push(
                        crate::Statement::Store {
                            pointer: target_handle,
                            value: value_expr,
                        },
                        span,
                    );

                    emitter.start(ctx.expressions);
                }
                Op::ControlBarrier => {
                    inst.expect(4)?;
                    let exec_scope_id = self.next()?;
                    let _mem_scope_raw = self.next()?;
                    let semantics_id = self.next()?;
                    let exec_scope_const = self.lookup_constant.lookup(exec_scope_id)?;
                    let semantics_const = self.lookup_constant.lookup(semantics_id)?;

                    let exec_scope = resolve_constant(ctx.gctx(), &exec_scope_const.inner)
                        .ok_or(Error::InvalidBarrierScope(exec_scope_id))?;
                    let semantics = resolve_constant(ctx.gctx(), &semantics_const.inner)
                        .ok_or(Error::InvalidBarrierMemorySemantics(semantics_id))?;

                    if exec_scope == spirv::Scope::Workgroup as u32
                        || exec_scope == spirv::Scope::Subgroup as u32
                    {
                        let mut flags = crate::Barrier::empty();
                        flags.set(
                            crate::Barrier::STORAGE,
                            semantics & spirv::MemorySemantics::UNIFORM_MEMORY.bits() != 0,
                        );
                        flags.set(
                            crate::Barrier::WORK_GROUP,
                            semantics & (spirv::MemorySemantics::WORKGROUP_MEMORY).bits() != 0,
                        );
                        flags.set(
                            crate::Barrier::SUB_GROUP,
                            semantics & spirv::MemorySemantics::SUBGROUP_MEMORY.bits() != 0,
                        );
                        flags.set(
                            crate::Barrier::TEXTURE,
                            semantics & spirv::MemorySemantics::IMAGE_MEMORY.bits() != 0,
                        );
                        block.push(crate::Statement::ControlBarrier(flags), span);
                    } else {
                        log::warn!("Unsupported barrier execution scope: {}", exec_scope);
                    }
                }
                Op::MemoryBarrier => {
                    inst.expect(3)?;
                    let mem_scope_id = self.next()?;
                    let semantics_id = self.next()?;
                    let mem_scope_const = self.lookup_constant.lookup(mem_scope_id)?;
                    let semantics_const = self.lookup_constant.lookup(semantics_id)?;

                    let mem_scope = resolve_constant(ctx.gctx(), &mem_scope_const.inner)
                        .ok_or(Error::InvalidBarrierScope(mem_scope_id))?;
                    let semantics = resolve_constant(ctx.gctx(), &semantics_const.inner)
                        .ok_or(Error::InvalidBarrierMemorySemantics(semantics_id))?;

                    let mut flags = if mem_scope == spirv::Scope::Device as u32 {
                        crate::Barrier::STORAGE
                    } else if mem_scope == spirv::Scope::Workgroup as u32 {
                        crate::Barrier::WORK_GROUP
                    } else if mem_scope == spirv::Scope::Subgroup as u32 {
                        crate::Barrier::SUB_GROUP
                    } else {
                        crate::Barrier::empty()
                    };
                    flags.set(
                        crate::Barrier::STORAGE,
                        semantics & spirv::MemorySemantics::UNIFORM_MEMORY.bits() != 0,
                    );
                    flags.set(
                        crate::Barrier::WORK_GROUP,
                        semantics & (spirv::MemorySemantics::WORKGROUP_MEMORY).bits() != 0,
                    );
                    flags.set(
                        crate::Barrier::SUB_GROUP,
                        semantics & spirv::MemorySemantics::SUBGROUP_MEMORY.bits() != 0,
                    );
                    flags.set(
                        crate::Barrier::TEXTURE,
                        semantics & spirv::MemorySemantics::IMAGE_MEMORY.bits() != 0,
                    );
                    block.push(crate::Statement::MemoryBarrier(flags), span);
                }
                Op::CopyObject => {
                    inst.expect(4)?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let operand_id = self.next()?;

                    let lookup = self.lookup_expression.lookup(operand_id)?;
                    let handle = get_expr_handle!(operand_id, lookup);

                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );
                }
                Op::GroupNonUniformBallot => {
                    inst.expect(5)?;
                    block.extend(emitter.finish(ctx.expressions));
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let exec_scope_id = self.next()?;
                    let predicate_id = self.next()?;

                    let exec_scope_const = self.lookup_constant.lookup(exec_scope_id)?;
                    let _exec_scope = resolve_constant(ctx.gctx(), &exec_scope_const.inner)
                        .filter(|exec_scope| *exec_scope == spirv::Scope::Subgroup as u32)
                        .ok_or(Error::InvalidBarrierScope(exec_scope_id))?;

                    let predicate = if self
                        .lookup_constant
                        .lookup(predicate_id)
                        .ok()
                        .filter(|predicate_const| match predicate_const.inner {
                            Constant::Constant(constant) => matches!(
                                ctx.gctx().global_expressions[ctx.gctx().constants[constant].init],
                                crate::Expression::Literal(crate::Literal::Bool(true)),
                            ),
                            Constant::Override(_) => false,
                        })
                        .is_some()
                    {
                        None
                    } else {
                        let predicate_lookup = self.lookup_expression.lookup(predicate_id)?;
                        let predicate_handle = get_expr_handle!(predicate_id, predicate_lookup);
                        Some(predicate_handle)
                    };

                    let result_handle = ctx
                        .expressions
                        .append(crate::Expression::SubgroupBallotResult, span);
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: result_handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );

                    block.push(
                        crate::Statement::SubgroupBallot {
                            result: result_handle,
                            predicate,
                        },
                        span,
                    );
                    emitter.start(ctx.expressions);
                }
                Op::GroupNonUniformAll
                | Op::GroupNonUniformAny
                | Op::GroupNonUniformIAdd
                | Op::GroupNonUniformFAdd
                | Op::GroupNonUniformIMul
                | Op::GroupNonUniformFMul
                | Op::GroupNonUniformSMax
                | Op::GroupNonUniformUMax
                | Op::GroupNonUniformFMax
                | Op::GroupNonUniformSMin
                | Op::GroupNonUniformUMin
                | Op::GroupNonUniformFMin
                | Op::GroupNonUniformBitwiseAnd
                | Op::GroupNonUniformBitwiseOr
                | Op::GroupNonUniformBitwiseXor
                | Op::GroupNonUniformLogicalAnd
                | Op::GroupNonUniformLogicalOr
                | Op::GroupNonUniformLogicalXor => {
                    block.extend(emitter.finish(ctx.expressions));
                    inst.expect(
                        if matches!(inst.op, Op::GroupNonUniformAll | Op::GroupNonUniformAny) {
                            5
                        } else {
                            6
                        },
                    )?;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let exec_scope_id = self.next()?;
                    let collective_op_id = match inst.op {
                        Op::GroupNonUniformAll | Op::GroupNonUniformAny => {
                            crate::CollectiveOperation::Reduce
                        }
                        _ => {
                            let group_op_id = self.next()?;
                            match spirv::GroupOperation::from_u32(group_op_id) {
                                Some(spirv::GroupOperation::Reduce) => {
                                    crate::CollectiveOperation::Reduce
                                }
                                Some(spirv::GroupOperation::InclusiveScan) => {
                                    crate::CollectiveOperation::InclusiveScan
                                }
                                Some(spirv::GroupOperation::ExclusiveScan) => {
                                    crate::CollectiveOperation::ExclusiveScan
                                }
                                _ => return Err(Error::UnsupportedGroupOperation(group_op_id)),
                            }
                        }
                    };
                    let argument_id = self.next()?;

                    let argument_lookup = self.lookup_expression.lookup(argument_id)?;
                    let argument_handle = get_expr_handle!(argument_id, argument_lookup);

                    let exec_scope_const = self.lookup_constant.lookup(exec_scope_id)?;
                    let _exec_scope = resolve_constant(ctx.gctx(), &exec_scope_const.inner)
                        .filter(|exec_scope| *exec_scope == spirv::Scope::Subgroup as u32)
                        .ok_or(Error::InvalidBarrierScope(exec_scope_id))?;

                    let op_id = match inst.op {
                        Op::GroupNonUniformAll => crate::SubgroupOperation::All,
                        Op::GroupNonUniformAny => crate::SubgroupOperation::Any,
                        Op::GroupNonUniformIAdd | Op::GroupNonUniformFAdd => {
                            crate::SubgroupOperation::Add
                        }
                        Op::GroupNonUniformIMul | Op::GroupNonUniformFMul => {
                            crate::SubgroupOperation::Mul
                        }
                        Op::GroupNonUniformSMax
                        | Op::GroupNonUniformUMax
                        | Op::GroupNonUniformFMax => crate::SubgroupOperation::Max,
                        Op::GroupNonUniformSMin
                        | Op::GroupNonUniformUMin
                        | Op::GroupNonUniformFMin => crate::SubgroupOperation::Min,
                        Op::GroupNonUniformBitwiseAnd | Op::GroupNonUniformLogicalAnd => {
                            crate::SubgroupOperation::And
                        }
                        Op::GroupNonUniformBitwiseOr | Op::GroupNonUniformLogicalOr => {
                            crate::SubgroupOperation::Or
                        }
                        Op::GroupNonUniformBitwiseXor | Op::GroupNonUniformLogicalXor => {
                            crate::SubgroupOperation::Xor
                        }
                        _ => unreachable!(),
                    };

                    let result_type = self.lookup_type.lookup(result_type_id)?;

                    let result_handle = ctx.expressions.append(
                        crate::Expression::SubgroupOperationResult {
                            ty: result_type.handle,
                        },
                        span,
                    );
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: result_handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );

                    block.push(
                        crate::Statement::SubgroupCollectiveOperation {
                            result: result_handle,
                            op: op_id,
                            collective_op: collective_op_id,
                            argument: argument_handle,
                        },
                        span,
                    );
                    emitter.start(ctx.expressions);
                }
                Op::GroupNonUniformBroadcastFirst
                | Op::GroupNonUniformBroadcast
                | Op::GroupNonUniformShuffle
                | Op::GroupNonUniformShuffleDown
                | Op::GroupNonUniformShuffleUp
                | Op::GroupNonUniformShuffleXor
                | Op::GroupNonUniformQuadBroadcast => {
                    inst.expect(if matches!(inst.op, Op::GroupNonUniformBroadcastFirst) {
                        5
                    } else {
                        6
                    })?;
                    block.extend(emitter.finish(ctx.expressions));
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let exec_scope_id = self.next()?;
                    let argument_id = self.next()?;

                    let argument_lookup = self.lookup_expression.lookup(argument_id)?;
                    let argument_handle = get_expr_handle!(argument_id, argument_lookup);

                    let exec_scope_const = self.lookup_constant.lookup(exec_scope_id)?;
                    let _exec_scope = resolve_constant(ctx.gctx(), &exec_scope_const.inner)
                        .filter(|exec_scope| *exec_scope == spirv::Scope::Subgroup as u32)
                        .ok_or(Error::InvalidBarrierScope(exec_scope_id))?;

                    let mode = if matches!(inst.op, Op::GroupNonUniformBroadcastFirst) {
                        crate::GatherMode::BroadcastFirst
                    } else {
                        let index_id = self.next()?;
                        let index_lookup = self.lookup_expression.lookup(index_id)?;
                        let index_handle = get_expr_handle!(index_id, index_lookup);
                        match inst.op {
                            Op::GroupNonUniformBroadcast => {
                                crate::GatherMode::Broadcast(index_handle)
                            }
                            Op::GroupNonUniformShuffle => crate::GatherMode::Shuffle(index_handle),
                            Op::GroupNonUniformShuffleDown => {
                                crate::GatherMode::ShuffleDown(index_handle)
                            }
                            Op::GroupNonUniformShuffleUp => {
                                crate::GatherMode::ShuffleUp(index_handle)
                            }
                            Op::GroupNonUniformShuffleXor => {
                                crate::GatherMode::ShuffleXor(index_handle)
                            }
                            Op::GroupNonUniformQuadBroadcast => {
                                crate::GatherMode::QuadBroadcast(index_handle)
                            }
                            _ => unreachable!(),
                        }
                    };

                    let result_type = self.lookup_type.lookup(result_type_id)?;

                    let result_handle = ctx.expressions.append(
                        crate::Expression::SubgroupOperationResult {
                            ty: result_type.handle,
                        },
                        span,
                    );
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: result_handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );

                    block.push(
                        crate::Statement::SubgroupGather {
                            result: result_handle,
                            mode,
                            argument: argument_handle,
                        },
                        span,
                    );
                    emitter.start(ctx.expressions);
                }
                Op::GroupNonUniformQuadSwap => {
                    inst.expect(6)?;
                    block.extend(emitter.finish(ctx.expressions));
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let exec_scope_id = self.next()?;
                    let argument_id = self.next()?;
                    let direction_id = self.next()?;

                    let argument_lookup = self.lookup_expression.lookup(argument_id)?;
                    let argument_handle = get_expr_handle!(argument_id, argument_lookup);

                    let exec_scope_const = self.lookup_constant.lookup(exec_scope_id)?;
                    let _exec_scope = resolve_constant(ctx.gctx(), &exec_scope_const.inner)
                        .filter(|exec_scope| *exec_scope == spirv::Scope::Subgroup as u32)
                        .ok_or(Error::InvalidBarrierScope(exec_scope_id))?;

                    let direction_const = self.lookup_constant.lookup(direction_id)?;
                    let direction_const = resolve_constant(ctx.gctx(), &direction_const.inner)
                        .ok_or(Error::InvalidOperand)?;
                    let direction = match direction_const {
                        0 => crate::Direction::X,
                        1 => crate::Direction::Y,
                        2 => crate::Direction::Diagonal,
                        _ => unreachable!(),
                    };

                    let result_type = self.lookup_type.lookup(result_type_id)?;

                    let result_handle = ctx.expressions.append(
                        crate::Expression::SubgroupOperationResult {
                            ty: result_type.handle,
                        },
                        span,
                    );
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle: result_handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );

                    block.push(
                        crate::Statement::SubgroupGather {
                            mode: crate::GatherMode::QuadSwap(direction),
                            result: result_handle,
                            argument: argument_handle,
                        },
                        span,
                    );
                    emitter.start(ctx.expressions);
                }
                Op::AtomicLoad => {
                    inst.expect(6)?;
                    let start = self.data_offset;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let pointer_id = self.next()?;
                    let _scope_id = self.next()?;
                    let _memory_semantics_id = self.next()?;
                    let span = self.span_from_with_op(start);

                    log::trace!("\t\t\tlooking up expr {:?}", pointer_id);
                    let p_lexp_handle =
                        get_expr_handle!(pointer_id, self.lookup_expression.lookup(pointer_id)?);

                    // Create an expression for our result
                    let expr = crate::Expression::Load {
                        pointer: p_lexp_handle,
                    };
                    let handle = ctx.expressions.append(expr, span);
                    self.lookup_expression.insert(
                        result_id,
                        LookupExpression {
                            handle,
                            type_id: result_type_id,
                            block_id,
                        },
                    );

                    // Store any associated global variables so we can upgrade their types later
                    self.record_atomic_access(ctx, p_lexp_handle)?;
                }
                Op::AtomicStore => {
                    inst.expect(5)?;
                    let start = self.data_offset;
                    let pointer_id = self.next()?;
                    let _scope_id = self.next()?;
                    let _memory_semantics_id = self.next()?;
                    let value_id = self.next()?;
                    let span = self.span_from_with_op(start);

                    log::trace!("\t\t\tlooking up pointer expr {:?}", pointer_id);
                    let p_lexp_handle =
                        get_expr_handle!(pointer_id, self.lookup_expression.lookup(pointer_id)?);

                    log::trace!("\t\t\tlooking up value expr {:?}", pointer_id);
                    let v_lexp_handle =
                        get_expr_handle!(value_id, self.lookup_expression.lookup(value_id)?);

                    block.extend(emitter.finish(ctx.expressions));
                    // Create a statement for the op itself
                    let stmt = crate::Statement::Store {
                        pointer: p_lexp_handle,
                        value: v_lexp_handle,
                    };
                    block.push(stmt, span);
                    emitter.start(ctx.expressions);

                    // Store any associated global variables so we can upgrade their types later
                    self.record_atomic_access(ctx, p_lexp_handle)?;
                }
                Op::AtomicIIncrement | Op::AtomicIDecrement => {
                    inst.expect(6)?;
                    let start = self.data_offset;
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let pointer_id = self.next()?;
                    let _scope_id = self.next()?;
                    let _memory_semantics_id = self.next()?;
                    let span = self.span_from_with_op(start);

                    let (p_exp_h, p_base_ty_h) = self.get_exp_and_base_ty_handles(
                        pointer_id,
                        ctx,
                        &mut emitter,
                        &mut block,
                        body_idx,
                    )?;

                    block.extend(emitter.finish(ctx.expressions));
                    // Create an expression for our result
                    let r_lexp_handle = {
                        let expr = crate::Expression::AtomicResult {
                            ty: p_base_ty_h,
                            comparison: false,
                        };
                        let handle = ctx.expressions.append(expr, span);
                        self.lookup_expression.insert(
                            result_id,
                            LookupExpression {
                                handle,
                                type_id: result_type_id,
                                block_id,
                            },
                        );
                        handle
                    };
                    emitter.start(ctx.expressions);

                    // Create a literal "1" to use as our value
                    let one_lexp_handle = make_index_literal(
                        ctx,
                        1,
                        &mut block,
                        &mut emitter,
                        p_base_ty_h,
                        result_type_id,
                        span,
                    )?;

                    // Create a statement for the op itself
                    let stmt = crate::Statement::Atomic {
                        pointer: p_exp_h,
                        fun: match inst.op {
                            Op::AtomicIIncrement => crate::AtomicFunction::Add,
                            _ => crate::AtomicFunction::Subtract,
                        },
                        value: one_lexp_handle,
                        result: Some(r_lexp_handle),
                    };
                    block.push(stmt, span);

                    // Store any associated global variables so we can upgrade their types later
                    self.record_atomic_access(ctx, p_exp_h)?;
                }
                Op::AtomicCompareExchange => {
                    inst.expect(9)?;

                    let start = self.data_offset;
                    let span = self.span_from_with_op(start);
                    let result_type_id = self.next()?;
                    let result_id = self.next()?;
                    let pointer_id = self.next()?;
                    let _memory_scope_id = self.next()?;
                    let _equal_memory_semantics_id = self.next()?;
                    let _unequal_memory_semantics_id = self.next()?;
                    let value_id = self.next()?;
                    let comparator_id = self.next()?;

                    let (p_exp_h, p_base_ty_h) = self.get_exp_and_base_ty_handles(
                        pointer_id,
                        ctx,
                        &mut emitter,
                        &mut block,
                        body_idx,
                    )?;

                    log::trace!("\t\t\tlooking up value expr {:?}", value_id);
                    let v_lexp_handle =
                        get_expr_handle!(value_id, self.lookup_expression.lookup(value_id)?);

                    log::trace!("\t\t\tlooking up comparator expr {:?}", value_id);
                    let c_lexp_handle = get_expr_handle!(
                        comparator_id,
                        self.lookup_expression.lookup(comparator_id)?
                    );

                    // We know from the SPIR-V spec that the result type must be an integer
                    // scalar, and we'll need the type itself to get a handle to the atomic
                    // result struct.
                    let crate::TypeInner::Scalar(scalar) = ctx.module.types[p_base_ty_h].inner
                    else {
                        return Err(
                            crate::front::atomic_upgrade::Error::CompareExchangeNonScalarBaseType
                                .into(),
                        );
                    };

                    // Get a handle to the atomic result struct type.
                    let atomic_result_struct_ty_h = ctx.module.generate_predeclared_type(
                        crate::PredeclaredType::AtomicCompareExchangeWeakResult(scalar),
                    );

                    block.extend(emitter.finish(ctx.expressions));

                    // Create an expression for our atomic result
                    let atomic_lexp_handle = {
                        let expr = crate::Expression::AtomicResult {
                            ty: atomic_result_struct_ty_h,
                            comparison: true,
                        };
                        ctx.expressions.append(expr, span)
                    };

                    // Create an dot accessor to extract the value from the
                    // result struct __atomic_compare_exchange_result<T> and use that
                    // as the expression for the result_id
                    {
                        let expr = crate::Expression::AccessIndex {
                            base: atomic_lexp_handle,
                            index: 0,
                        };
                        let handle = ctx.expressions.append(expr, span);
                        // Use this dot accessor as the result id's expression
                        let _ = self.lookup_expression.insert(
                            result_id,
                            LookupExpression {
                                handle,
                                type_id: result_type_id,
                                block_id,
                            },
                        );
                    }

                    emitter.start(ctx.expressions);

                    // Create a statement for the op itself
                    let stmt = crate::Statement::Atomic {
                        pointer: p_exp_h,
                        fun: crate::AtomicFunction::Exchange {
                            compare: Some(c_lexp_handle),
                        },
                        value: v_lexp_handle,
                        result: Some(atomic_lexp_handle),
                    };
                    block.push(stmt, span);

                    // Store any associated global variables so we can upgrade their types later
                    self.record_atomic_access(ctx, p_exp_h)?;
                }
                Op::AtomicExchange
                | Op::AtomicIAdd
                | Op::AtomicISub
                | Op::AtomicSMin
                | Op::AtomicUMin
                | Op::AtomicSMax
                | Op::AtomicUMax
                | Op::AtomicAnd
                | Op::AtomicOr
                | Op::AtomicXor
                | Op::AtomicFAddEXT => self.parse_atomic_expr_with_value(
                    inst,
                    &mut emitter,
                    ctx,
                    &mut block,
                    block_id,
                    body_idx,
                    match inst.op {
                        Op::AtomicExchange => crate::AtomicFunction::Exchange { compare: None },
                        Op::AtomicIAdd | Op::AtomicFAddEXT => crate::AtomicFunction::Add,
                        Op::AtomicISub => crate::AtomicFunction::Subtract,
                        Op::AtomicSMin => crate::AtomicFunction::Min,
                        Op::AtomicUMin => crate::AtomicFunction::Min,
                        Op::AtomicSMax => crate::AtomicFunction::Max,
                        Op::AtomicUMax => crate::AtomicFunction::Max,
                        Op::AtomicAnd => crate::AtomicFunction::And,
                        Op::AtomicOr => crate::AtomicFunction::InclusiveOr,
                        Op::AtomicXor => crate::AtomicFunction::ExclusiveOr,
                        _ => unreachable!(),
                    },
                )?,

                _ => {
                    return Err(Error::UnsupportedInstruction(self.state, inst.op));
                }
            }
        };

        block.extend(emitter.finish(ctx.expressions));
        if let Some(stmt) = terminator {
            block.push(stmt, crate::Span::default());
        }

        // Save this block fragment in `block_ctx.blocks`, and mark it to be
        // incorporated into the current body at `Statement` assembly time.
        ctx.blocks.insert(block_id, block);
        let body = &mut ctx.bodies[body_idx];
        body.data.push(BodyFragment::BlockId(block_id));
        Ok(())
    }

    fn make_expression_storage(
        &mut self,
        globals: &Arena<crate::GlobalVariable>,
        constants: &Arena<crate::Constant>,
        overrides: &Arena<crate::Override>,
    ) -> Arena<crate::Expression> {
        let mut expressions = Arena::new();
        #[allow(clippy::panic)]
        {
            assert!(self.lookup_expression.is_empty());
        }
        // register global variables
        for (&id, var) in self.lookup_variable.iter() {
            let span = globals.get_span(var.handle);
            let handle = expressions.append(crate::Expression::GlobalVariable(var.handle), span);
            self.lookup_expression.insert(
                id,
                LookupExpression {
                    type_id: var.type_id,
                    handle,
                    // Setting this to an invalid id will cause get_expr_handle
                    // to default to the main body making sure no load/stores
                    // are added.
                    block_id: 0,
                },
            );
        }
        // register constants
        for (&id, con) in self.lookup_constant.iter() {
            let (expr, span) = match con.inner {
                Constant::Constant(c) => (crate::Expression::Constant(c), constants.get_span(c)),
                Constant::Override(o) => (crate::Expression::Override(o), overrides.get_span(o)),
            };
            let handle = expressions.append(expr, span);
            self.lookup_expression.insert(
                id,
                LookupExpression {
                    type_id: con.type_id,
                    handle,
                    // Setting this to an invalid id will cause get_expr_handle
                    // to default to the main body making sure no load/stores
                    // are added.
                    block_id: 0,
                },
            );
        }
        // done
        expressions
    }

    fn switch(&mut self, state: ModuleState, op: spirv::Op) -> Result<(), Error> {
        if state < self.state {
            Err(Error::UnsupportedInstruction(self.state, op))
        } else {
            self.state = state;
            Ok(())
        }
    }

    /// Walk the statement tree and patch it in the following cases:
    /// 1. Function call targets are replaced by `deferred_function_calls` map
    fn patch_statements(
        &mut self,
        statements: &mut crate::Block,
        expressions: &mut Arena<crate::Expression>,
        fun_parameter_sampling: &mut [image::SamplingFlags],
    ) -> Result<(), Error> {
        use crate::Statement as S;
        let mut i = 0usize;
        while i < statements.len() {
            match statements[i] {
                S::Emit(_) => {}
                S::Block(ref mut block) => {
                    self.patch_statements(block, expressions, fun_parameter_sampling)?;
                }
                S::If {
                    condition: _,
                    ref mut accept,
                    ref mut reject,
                } => {
                    self.patch_statements(reject, expressions, fun_parameter_sampling)?;
                    self.patch_statements(accept, expressions, fun_parameter_sampling)?;
                }
                S::Switch {
                    selector: _,
                    ref mut cases,
                } => {
                    for case in cases.iter_mut() {
                        self.patch_statements(&mut case.body, expressions, fun_parameter_sampling)?;
                    }
                }
                S::Loop {
                    ref mut body,
                    ref mut continuing,
                    break_if: _,
                } => {
                    self.patch_statements(body, expressions, fun_parameter_sampling)?;
                    self.patch_statements(continuing, expressions, fun_parameter_sampling)?;
                }
                S::Break
                | S::Continue
                | S::Return { .. }
                | S::Kill
                | S::ControlBarrier(_)
                | S::MemoryBarrier(_)
                | S::Store { .. }
                | S::ImageStore { .. }
                | S::Atomic { .. }
                | S::ImageAtomic { .. }
                | S::RayQuery { .. }
                | S::SubgroupBallot { .. }
                | S::SubgroupCollectiveOperation { .. }
                | S::SubgroupGather { .. } => {}
                S::Call {
                    function: ref mut callee,
                    ref arguments,
                    ..
                } => {
                    let fun_id = self.deferred_function_calls[callee.index()];
                    let fun_lookup = self.lookup_function.lookup(fun_id)?;
                    *callee = fun_lookup.handle;

                    // Patch sampling flags
                    for (arg_index, arg) in arguments.iter().enumerate() {
                        let flags = match fun_lookup.parameters_sampling.get(arg_index) {
                            Some(&flags) if !flags.is_empty() => flags,
                            _ => continue,
                        };

                        match expressions[*arg] {
                            crate::Expression::GlobalVariable(handle) => {
                                if let Some(sampling) = self.handle_sampling.get_mut(&handle) {
                                    *sampling |= flags
                                }
                            }
                            crate::Expression::FunctionArgument(i) => {
                                fun_parameter_sampling[i as usize] |= flags;
                            }
                            ref other => return Err(Error::InvalidGlobalVar(other.clone())),
                        }
                    }
                }
                S::WorkGroupUniformLoad { .. } => unreachable!(),
            }
            i += 1;
        }
        Ok(())
    }

    fn patch_function(
        &mut self,
        handle: Option<Handle<crate::Function>>,
        fun: &mut crate::Function,
    ) -> Result<(), Error> {
        // Note: this search is a bit unfortunate
        let (fun_id, mut parameters_sampling) = match handle {
            Some(h) => {
                let (&fun_id, lookup) = self
                    .lookup_function
                    .iter_mut()
                    .find(|&(_, ref lookup)| lookup.handle == h)
                    .unwrap();
                (fun_id, mem::take(&mut lookup.parameters_sampling))
            }
            None => (0, Vec::new()),
        };

        for (_, expr) in fun.expressions.iter_mut() {
            if let crate::Expression::CallResult(ref mut function) = *expr {
                let fun_id = self.deferred_function_calls[function.index()];
                *function = self.lookup_function.lookup(fun_id)?.handle;
            }
        }

        self.patch_statements(
            &mut fun.body,
            &mut fun.expressions,
            &mut parameters_sampling,
        )?;

        if let Some(lookup) = self.lookup_function.get_mut(&fun_id) {
            lookup.parameters_sampling = parameters_sampling;
        }
        Ok(())
    }

    pub fn parse(mut self) -> Result<crate::Module, Error> {
        let mut module = {
            if self.next()? != spirv::MAGIC_NUMBER {
                return Err(Error::InvalidHeader);
            }
            let version_raw = self.next()?;
            let generator = self.next()?;
            let _bound = self.next()?;
            let _schema = self.next()?;
            log::info!("Generated by {} version {:x}", generator, version_raw);
            crate::Module::default()
        };

        self.layouter.clear();
        self.dummy_functions = Arena::new();
        self.lookup_function.clear();
        self.function_call_graph.clear();

        loop {
            use spirv::Op;

            let inst = match self.next_inst() {
                Ok(inst) => inst,
                Err(Error::IncompleteData) => break,
                Err(other) => return Err(other),
            };
            log::debug!("\t{:?} [{}]", inst.op, inst.wc);

            match inst.op {
                Op::Capability => self.parse_capability(inst),
                Op::Extension => self.parse_extension(inst),
                Op::ExtInstImport => self.parse_ext_inst_import(inst),
                Op::MemoryModel => self.parse_memory_model(inst),
                Op::EntryPoint => self.parse_entry_point(inst),
                Op::ExecutionMode => self.parse_execution_mode(inst),
                Op::String => self.parse_string(inst),
                Op::Source => self.parse_source(inst),
                Op::SourceExtension => self.parse_source_extension(inst),
                Op::Name => self.parse_name(inst),
                Op::MemberName => self.parse_member_name(inst),
                Op::ModuleProcessed => self.parse_module_processed(inst),
                Op::Decorate => self.parse_decorate(inst),
                Op::MemberDecorate => self.parse_member_decorate(inst),
                Op::TypeVoid => self.parse_type_void(inst),
                Op::TypeBool => self.parse_type_bool(inst, &mut module),
                Op::TypeInt => self.parse_type_int(inst, &mut module),
                Op::TypeFloat => self.parse_type_float(inst, &mut module),
                Op::TypeVector => self.parse_type_vector(inst, &mut module),
                Op::TypeMatrix => self.parse_type_matrix(inst, &mut module),
                Op::TypeFunction => self.parse_type_function(inst),
                Op::TypePointer => self.parse_type_pointer(inst, &mut module),
                Op::TypeArray => self.parse_type_array(inst, &mut module),
                Op::TypeRuntimeArray => self.parse_type_runtime_array(inst, &mut module),
                Op::TypeStruct => self.parse_type_struct(inst, &mut module),
                Op::TypeImage => self.parse_type_image(inst, &mut module),
                Op::TypeSampledImage => self.parse_type_sampled_image(inst),
                Op::TypeSampler => self.parse_type_sampler(inst, &mut module),
                Op::Constant | Op::SpecConstant => self.parse_constant(inst, &mut module),
                Op::ConstantComposite | Op::SpecConstantComposite => {
                    self.parse_composite_constant(inst, &mut module)
                }
                Op::ConstantNull | Op::Undef => self.parse_null_constant(inst, &mut module),
                Op::ConstantTrue | Op::SpecConstantTrue => {
                    self.parse_bool_constant(inst, true, &mut module)
                }
                Op::ConstantFalse | Op::SpecConstantFalse => {
                    self.parse_bool_constant(inst, false, &mut module)
                }
                Op::Variable => self.parse_global_variable(inst, &mut module),
                Op::Function => {
                    self.switch(ModuleState::Function, inst.op)?;
                    inst.expect(5)?;
                    self.parse_function(&mut module)
                }
                _ => Err(Error::UnsupportedInstruction(self.state, inst.op)), //TODO
            }?;
        }

        if !self.upgrade_atomics.is_empty() {
            log::info!("Upgrading atomic pointers...");
            module.upgrade_atomics(&self.upgrade_atomics)?;
        }

        // Do entry point specific processing after all functions are parsed so that we can
        // cull unused problematic builtins of gl_PerVertex.
        for (ep, fun_id) in mem::take(&mut self.deferred_entry_points) {
            self.process_entry_point(&mut module, ep, fun_id)?;
        }

        log::info!("Patching...");
        {
            let mut nodes = petgraph::algo::toposort(&self.function_call_graph, None)
                .map_err(|cycle| Error::FunctionCallCycle(cycle.node_id()))?;
            nodes.reverse(); // we need dominated first
            let mut functions = mem::take(&mut module.functions);
            for fun_id in nodes {
                if fun_id > !(functions.len() as u32) {
                    // skip all the fake IDs registered for the entry points
                    continue;
                }
                let lookup = self.lookup_function.get_mut(&fun_id).unwrap();
                // take out the function from the old array
                let fun = mem::take(&mut functions[lookup.handle]);
                // add it to the newly formed arena, and adjust the lookup
                lookup.handle = module
                    .functions
                    .append(fun, functions.get_span(lookup.handle));
            }
        }
        // patch all the functions
        for (handle, fun) in module.functions.iter_mut() {
            self.patch_function(Some(handle), fun)?;
        }
        for ep in module.entry_points.iter_mut() {
            self.patch_function(None, &mut ep.function)?;
        }

        // Check all the images and samplers to have consistent comparison property.
        for (handle, flags) in self.handle_sampling.drain() {
            if !image::patch_comparison_type(
                flags,
                module.global_variables.get_mut(handle),
                &mut module.types,
            ) {
                return Err(Error::InconsistentComparisonSampling(handle));
            }
        }

        if !self.future_decor.is_empty() {
            log::warn!("Unused item decorations: {:?}", self.future_decor);
            self.future_decor.clear();
        }
        if !self.future_member_decor.is_empty() {
            log::warn!("Unused member decorations: {:?}", self.future_member_decor);
            self.future_member_decor.clear();
        }

        Ok(module)
    }

    fn parse_capability(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Capability, inst.op)?;
        inst.expect(2)?;
        let capability = self.next()?;
        let cap =
            spirv::Capability::from_u32(capability).ok_or(Error::UnknownCapability(capability))?;
        if !SUPPORTED_CAPABILITIES.contains(&cap) {
            if self.options.strict_capabilities {
                return Err(Error::UnsupportedCapability(cap));
            } else {
                log::warn!("Unknown capability {:?}", cap);
            }
        }
        Ok(())
    }

    fn parse_extension(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Extension, inst.op)?;
        inst.expect_at_least(2)?;
        let (name, left) = self.next_string(inst.wc - 1)?;
        if left != 0 {
            return Err(Error::InvalidOperand);
        }
        if !SUPPORTED_EXTENSIONS.contains(&name.as_str()) {
            return Err(Error::UnsupportedExtension(name));
        }
        Ok(())
    }

    fn parse_ext_inst_import(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Extension, inst.op)?;
        inst.expect_at_least(3)?;
        let result_id = self.next()?;
        let (name, left) = self.next_string(inst.wc - 2)?;
        if left != 0 {
            return Err(Error::InvalidOperand);
        }
        if !SUPPORTED_EXT_SETS.contains(&name.as_str()) {
            return Err(Error::UnsupportedExtSet(name));
        }
        self.ext_glsl_id = Some(result_id);
        Ok(())
    }

    fn parse_memory_model(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::MemoryModel, inst.op)?;
        inst.expect(3)?;
        let _addressing_model = self.next()?;
        let _memory_model = self.next()?;
        Ok(())
    }

    fn parse_entry_point(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::EntryPoint, inst.op)?;
        inst.expect_at_least(4)?;
        let exec_model = self.next()?;
        let exec_model = spirv::ExecutionModel::from_u32(exec_model)
            .ok_or(Error::UnsupportedExecutionModel(exec_model))?;
        let function_id = self.next()?;
        let (name, left) = self.next_string(inst.wc - 3)?;
        let ep = EntryPoint {
            stage: match exec_model {
                spirv::ExecutionModel::Vertex => crate::ShaderStage::Vertex,
                spirv::ExecutionModel::Fragment => crate::ShaderStage::Fragment,
                spirv::ExecutionModel::GLCompute => crate::ShaderStage::Compute,
                _ => return Err(Error::UnsupportedExecutionModel(exec_model as u32)),
            },
            name,
            early_depth_test: None,
            workgroup_size: [0; 3],
            variable_ids: self.data.by_ref().take(left as usize).collect(),
        };
        self.lookup_entry_point.insert(function_id, ep);
        Ok(())
    }

    fn parse_execution_mode(&mut self, inst: Instruction) -> Result<(), Error> {
        use spirv::ExecutionMode;

        self.switch(ModuleState::ExecutionMode, inst.op)?;
        inst.expect_at_least(3)?;

        let ep_id = self.next()?;
        let mode_id = self.next()?;
        let args: Vec<spirv::Word> = self.data.by_ref().take(inst.wc as usize - 3).collect();

        let ep = self
            .lookup_entry_point
            .get_mut(&ep_id)
            .ok_or(Error::InvalidId(ep_id))?;
        let mode =
            ExecutionMode::from_u32(mode_id).ok_or(Error::UnsupportedExecutionMode(mode_id))?;

        match mode {
            ExecutionMode::EarlyFragmentTests => {
                ep.early_depth_test = Some(crate::EarlyDepthTest::Force);
            }
            ExecutionMode::DepthUnchanged => {
                if let &mut Some(ref mut early_depth_test) = &mut ep.early_depth_test {
                    if let &mut crate::EarlyDepthTest::Allow {
                        ref mut conservative,
                    } = early_depth_test
                    {
                        *conservative = crate::ConservativeDepth::Unchanged;
                    }
                } else {
                    ep.early_depth_test = Some(crate::EarlyDepthTest::Allow {
                        conservative: crate::ConservativeDepth::Unchanged,
                    });
                }
            }
            ExecutionMode::DepthGreater => {
                if let &mut Some(ref mut early_depth_test) = &mut ep.early_depth_test {
                    if let &mut crate::EarlyDepthTest::Allow {
                        ref mut conservative,
                    } = early_depth_test
                    {
                        *conservative = crate::ConservativeDepth::GreaterEqual;
                    }
                } else {
                    ep.early_depth_test = Some(crate::EarlyDepthTest::Allow {
                        conservative: crate::ConservativeDepth::GreaterEqual,
                    });
                }
            }
            ExecutionMode::DepthLess => {
                if let &mut Some(ref mut early_depth_test) = &mut ep.early_depth_test {
                    if let &mut crate::EarlyDepthTest::Allow {
                        ref mut conservative,
                    } = early_depth_test
                    {
                        *conservative = crate::ConservativeDepth::LessEqual;
                    }
                } else {
                    ep.early_depth_test = Some(crate::EarlyDepthTest::Allow {
                        conservative: crate::ConservativeDepth::LessEqual,
                    });
                }
            }
            ExecutionMode::DepthReplacing => {
                // Ignored because it can be deduced from the IR.
            }
            ExecutionMode::OriginUpperLeft => {
                // Ignored because the other option (OriginLowerLeft) is not valid in Vulkan mode.
            }
            ExecutionMode::LocalSize => {
                ep.workgroup_size = [args[0], args[1], args[2]];
            }
            _ => {
                return Err(Error::UnsupportedExecutionMode(mode_id));
            }
        }

        Ok(())
    }

    fn parse_string(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Source, inst.op)?;
        inst.expect_at_least(3)?;
        let _id = self.next()?;
        let (_name, _) = self.next_string(inst.wc - 2)?;
        Ok(())
    }

    fn parse_source(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Source, inst.op)?;
        for _ in 1..inst.wc {
            let _ = self.next()?;
        }
        Ok(())
    }

    fn parse_source_extension(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Source, inst.op)?;
        inst.expect_at_least(2)?;
        let (_name, _) = self.next_string(inst.wc - 1)?;
        Ok(())
    }

    fn parse_name(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Name, inst.op)?;
        inst.expect_at_least(3)?;
        let id = self.next()?;
        let (name, left) = self.next_string(inst.wc - 2)?;
        if left != 0 {
            return Err(Error::InvalidOperand);
        }
        self.future_decor.entry(id).or_default().name = Some(name);
        Ok(())
    }

    fn parse_member_name(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Name, inst.op)?;
        inst.expect_at_least(4)?;
        let id = self.next()?;
        let member = self.next()?;
        let (name, left) = self.next_string(inst.wc - 3)?;
        if left != 0 {
            return Err(Error::InvalidOperand);
        }

        self.future_member_decor
            .entry((id, member))
            .or_default()
            .name = Some(name);
        Ok(())
    }

    fn parse_module_processed(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Name, inst.op)?;
        inst.expect_at_least(2)?;
        let (_info, left) = self.next_string(inst.wc - 1)?;
        //Note: string is ignored
        if left != 0 {
            return Err(Error::InvalidOperand);
        }
        Ok(())
    }

    fn parse_decorate(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Annotation, inst.op)?;
        inst.expect_at_least(3)?;
        let id = self.next()?;
        let mut dec = self.future_decor.remove(&id).unwrap_or_default();
        self.next_decoration(inst, 2, &mut dec)?;
        self.future_decor.insert(id, dec);
        Ok(())
    }

    fn parse_member_decorate(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Annotation, inst.op)?;
        inst.expect_at_least(4)?;
        let id = self.next()?;
        let member = self.next()?;

        let mut dec = self
            .future_member_decor
            .remove(&(id, member))
            .unwrap_or_default();
        self.next_decoration(inst, 3, &mut dec)?;
        self.future_member_decor.insert((id, member), dec);
        Ok(())
    }

    fn parse_type_void(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(2)?;
        let id = self.next()?;
        self.lookup_void_type = Some(id);
        Ok(())
    }

    fn parse_type_bool(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(2)?;
        let id = self.next()?;
        let inner = crate::TypeInner::Scalar(crate::Scalar::BOOL);
        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: self.future_decor.remove(&id).and_then(|dec| dec.name),
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: None,
            },
        );
        Ok(())
    }

    fn parse_type_int(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(4)?;
        let id = self.next()?;
        let width = self.next()?;
        let sign = self.next()?;
        let inner = crate::TypeInner::Scalar(crate::Scalar {
            kind: match sign {
                0 => crate::ScalarKind::Uint,
                1 => crate::ScalarKind::Sint,
                _ => return Err(Error::InvalidSign(sign)),
            },
            width: map_width(width)?,
        });
        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: self.future_decor.remove(&id).and_then(|dec| dec.name),
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: None,
            },
        );
        Ok(())
    }

    fn parse_type_float(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(3)?;
        let id = self.next()?;
        let width = self.next()?;
        let inner = crate::TypeInner::Scalar(crate::Scalar::float(map_width(width)?));
        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: self.future_decor.remove(&id).and_then(|dec| dec.name),
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: None,
            },
        );
        Ok(())
    }

    fn parse_type_vector(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(4)?;
        let id = self.next()?;
        let type_id = self.next()?;
        let type_lookup = self.lookup_type.lookup(type_id)?;
        let scalar = match module.types[type_lookup.handle].inner {
            crate::TypeInner::Scalar(scalar) => scalar,
            _ => return Err(Error::InvalidInnerType(type_id)),
        };
        let component_count = self.next()?;
        let inner = crate::TypeInner::Vector {
            size: map_vector_size(component_count)?,
            scalar,
        };
        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: self.future_decor.remove(&id).and_then(|dec| dec.name),
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: Some(type_id),
            },
        );
        Ok(())
    }

    fn parse_type_matrix(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(4)?;
        let id = self.next()?;
        let vector_type_id = self.next()?;
        let num_columns = self.next()?;
        let decor = self.future_decor.remove(&id);

        let vector_type_lookup = self.lookup_type.lookup(vector_type_id)?;
        let inner = match module.types[vector_type_lookup.handle].inner {
            crate::TypeInner::Vector { size, scalar } => crate::TypeInner::Matrix {
                columns: map_vector_size(num_columns)?,
                rows: size,
                scalar,
            },
            _ => return Err(Error::InvalidInnerType(vector_type_id)),
        };

        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: decor.and_then(|dec| dec.name),
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: Some(vector_type_id),
            },
        );
        Ok(())
    }

    fn parse_type_function(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect_at_least(3)?;
        let id = self.next()?;
        let return_type_id = self.next()?;
        let parameter_type_ids = self.data.by_ref().take(inst.wc as usize - 3).collect();
        self.lookup_function_type.insert(
            id,
            LookupFunctionType {
                parameter_type_ids,
                return_type_id,
            },
        );
        Ok(())
    }

    fn parse_type_pointer(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(4)?;
        let id = self.next()?;
        let storage_class = self.next()?;
        let type_id = self.next()?;

        let decor = self.future_decor.remove(&id);
        let base_lookup_ty = self.lookup_type.lookup(type_id)?;
        let base_inner = &module.types[base_lookup_ty.handle].inner;

        let space = if let Some(space) = base_inner.pointer_space() {
            space
        } else if self
            .lookup_storage_buffer_types
            .contains_key(&base_lookup_ty.handle)
        {
            crate::AddressSpace::Storage {
                access: crate::StorageAccess::default(),
            }
        } else {
            match map_storage_class(storage_class)? {
                ExtendedClass::Global(space) => space,
                ExtendedClass::Input | ExtendedClass::Output => crate::AddressSpace::Private,
            }
        };

        // We don't support pointers to runtime-sized arrays in the `Uniform`
        // storage class with the `BufferBlock` decoration. Runtime-sized arrays
        // should be in the StorageBuffer class.
        if let crate::TypeInner::Array {
            size: crate::ArraySize::Dynamic,
            ..
        } = *base_inner
        {
            match space {
                crate::AddressSpace::Storage { .. } => {}
                _ => {
                    return Err(Error::UnsupportedRuntimeArrayStorageClass);
                }
            }
        }

        // Don't bother with pointer stuff for `Handle` types.
        let lookup_ty = if space == crate::AddressSpace::Handle {
            base_lookup_ty.clone()
        } else {
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: decor.and_then(|dec| dec.name),
                        inner: crate::TypeInner::Pointer {
                            base: base_lookup_ty.handle,
                            space,
                        },
                    },
                    self.span_from_with_op(start),
                ),
                base_id: Some(type_id),
            }
        };
        self.lookup_type.insert(id, lookup_ty);
        Ok(())
    }

    fn parse_type_array(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(4)?;
        let id = self.next()?;
        let type_id = self.next()?;
        let length_id = self.next()?;
        let length_const = self.lookup_constant.lookup(length_id)?;

        let size = resolve_constant(module.to_ctx(), &length_const.inner)
            .and_then(NonZeroU32::new)
            .ok_or(Error::InvalidArraySize(length_id))?;

        let decor = self.future_decor.remove(&id).unwrap_or_default();
        let base = self.lookup_type.lookup(type_id)?.handle;

        self.layouter.update(module.to_ctx()).unwrap();

        // HACK if the underlying type is an image or a sampler, let's assume
        //      that we're dealing with a binding-array
        //
        // Note that it's not a strictly correct assumption, but rather a trade
        // off caused by an impedance mismatch between SPIR-V's and Naga's type
        // systems - Naga distinguishes between arrays and binding-arrays via
        // types (i.e. both kinds of arrays are just different types), while
        // SPIR-V distinguishes between them through usage - e.g. given:
        //
        // ```
        // %image = OpTypeImage %float 2D 2 0 0 2 Rgba16f
        // %uint_256 = OpConstant %uint 256
        // %image_array = OpTypeArray %image %uint_256
        // ```
        //
        // ```
        // %image = OpTypeImage %float 2D 2 0 0 2 Rgba16f
        // %uint_256 = OpConstant %uint 256
        // %image_array = OpTypeArray %image %uint_256
        // %image_array_ptr = OpTypePointer UniformConstant %image_array
        // ```
        //
        // ... in the first case, `%image_array` should technically correspond
        // to `TypeInner::Array`, while in the second case it should say
        // `TypeInner::BindingArray` (kinda, depending on whether `%image_array`
        // is ever used as a freestanding type or rather always through the
        // pointer-indirection).
        //
        // Anyway, at the moment we don't support other kinds of image / sampler
        // arrays than those binding-based, so this assumption is pretty safe
        // for now.
        let inner = if let crate::TypeInner::Image { .. } | crate::TypeInner::Sampler { .. } =
            module.types[base].inner
        {
            crate::TypeInner::BindingArray {
                base,
                size: crate::ArraySize::Constant(size),
            }
        } else {
            crate::TypeInner::Array {
                base,
                size: crate::ArraySize::Constant(size),
                stride: match decor.array_stride {
                    Some(stride) => stride.get(),
                    None => self.layouter[base].to_stride(),
                },
            }
        };

        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: decor.name,
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: Some(type_id),
            },
        );
        Ok(())
    }

    fn parse_type_runtime_array(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(3)?;
        let id = self.next()?;
        let type_id = self.next()?;

        let decor = self.future_decor.remove(&id).unwrap_or_default();
        let base = self.lookup_type.lookup(type_id)?.handle;

        self.layouter.update(module.to_ctx()).unwrap();

        // HACK same case as in `parse_type_array()`
        let inner = if let crate::TypeInner::Image { .. } | crate::TypeInner::Sampler { .. } =
            module.types[base].inner
        {
            crate::TypeInner::BindingArray {
                base: self.lookup_type.lookup(type_id)?.handle,
                size: crate::ArraySize::Dynamic,
            }
        } else {
            crate::TypeInner::Array {
                base: self.lookup_type.lookup(type_id)?.handle,
                size: crate::ArraySize::Dynamic,
                stride: match decor.array_stride {
                    Some(stride) => stride.get(),
                    None => self.layouter[base].to_stride(),
                },
            }
        };

        self.lookup_type.insert(
            id,
            LookupType {
                handle: module.types.insert(
                    crate::Type {
                        name: decor.name,
                        inner,
                    },
                    self.span_from_with_op(start),
                ),
                base_id: Some(type_id),
            },
        );
        Ok(())
    }

    fn parse_type_struct(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect_at_least(2)?;
        let id = self.next()?;
        let parent_decor = self.future_decor.remove(&id);
        let is_storage_buffer = parent_decor
            .as_ref()
            .is_some_and(|decor| decor.storage_buffer);

        self.layouter.update(module.to_ctx()).unwrap();

        let mut members = Vec::<crate::StructMember>::with_capacity(inst.wc as usize - 2);
        let mut member_lookups = Vec::with_capacity(members.capacity());
        let mut storage_access = crate::StorageAccess::empty();
        let mut span = 0;
        let mut alignment = Alignment::ONE;
        for i in 0..u32::from(inst.wc) - 2 {
            let type_id = self.next()?;
            let ty = self.lookup_type.lookup(type_id)?.handle;
            let decor = self
                .future_member_decor
                .remove(&(id, i))
                .unwrap_or_default();

            storage_access |= decor.flags.to_storage_access();

            member_lookups.push(LookupMember {
                type_id,
                row_major: decor.matrix_major == Some(Majority::Row),
            });

            let member_alignment = self.layouter[ty].alignment;
            span = member_alignment.round_up(span);
            alignment = member_alignment.max(alignment);

            let binding = decor.io_binding().ok();
            if let Some(offset) = decor.offset {
                span = offset;
            }
            let offset = span;

            span += self.layouter[ty].size;

            let inner = &module.types[ty].inner;
            if let crate::TypeInner::Matrix {
                columns,
                rows,
                scalar,
            } = *inner
            {
                if let Some(stride) = decor.matrix_stride {
                    let expected_stride = Alignment::from(rows) * scalar.width as u32;
                    if stride.get() != expected_stride {
                        return Err(Error::UnsupportedMatrixStride {
                            stride: stride.get(),
                            columns: columns as u8,
                            rows: rows as u8,
                            width: scalar.width,
                        });
                    }
                }
            }

            members.push(crate::StructMember {
                name: decor.name,
                ty,
                binding,
                offset,
            });
        }

        span = alignment.round_up(span);

        let inner = crate::TypeInner::Struct { span, members };

        let ty_handle = module.types.insert(
            crate::Type {
                name: parent_decor.and_then(|dec| dec.name),
                inner,
            },
            self.span_from_with_op(start),
        );

        if is_storage_buffer {
            self.lookup_storage_buffer_types
                .insert(ty_handle, storage_access);
        }
        for (i, member_lookup) in member_lookups.into_iter().enumerate() {
            self.lookup_member
                .insert((ty_handle, i as u32), member_lookup);
        }
        self.lookup_type.insert(
            id,
            LookupType {
                handle: ty_handle,
                base_id: None,
            },
        );
        Ok(())
    }

    fn parse_type_image(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(9)?;

        let id = self.next()?;
        let sample_type_id = self.next()?;
        let dim = self.next()?;
        let is_depth = self.next()?;
        let is_array = self.next()? != 0;
        let is_msaa = self.next()? != 0;
        let _is_sampled = self.next()?;
        let format = self.next()?;

        let dim = map_image_dim(dim)?;
        let decor = self.future_decor.remove(&id).unwrap_or_default();

        // ensure there is a type for texture coordinate without extra components
        module.types.insert(
            crate::Type {
                name: None,
                inner: {
                    let scalar = crate::Scalar::F32;
                    match dim.required_coordinate_size() {
                        None => crate::TypeInner::Scalar(scalar),
                        Some(size) => crate::TypeInner::Vector { size, scalar },
                    }
                },
            },
            Default::default(),
        );

        let base_handle = self.lookup_type.lookup(sample_type_id)?.handle;
        let kind = module.types[base_handle]
            .inner
            .scalar_kind()
            .ok_or(Error::InvalidImageBaseType(base_handle))?;

        let inner = crate::TypeInner::Image {
            class: if is_depth == 1 {
                crate::ImageClass::Depth { multi: is_msaa }
            } else if format != 0 {
                crate::ImageClass::Storage {
                    format: map_image_format(format)?,
                    access: crate::StorageAccess::default(),
                }
            } else {
                crate::ImageClass::Sampled {
                    kind,
                    multi: is_msaa,
                }
            },
            dim,
            arrayed: is_array,
        };

        let handle = module.types.insert(
            crate::Type {
                name: decor.name,
                inner,
            },
            self.span_from_with_op(start),
        );

        self.lookup_type.insert(
            id,
            LookupType {
                handle,
                base_id: Some(sample_type_id),
            },
        );
        Ok(())
    }

    fn parse_type_sampled_image(&mut self, inst: Instruction) -> Result<(), Error> {
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(3)?;
        let id = self.next()?;
        let image_id = self.next()?;
        self.lookup_type.insert(
            id,
            LookupType {
                handle: self.lookup_type.lookup(image_id)?.handle,
                base_id: Some(image_id),
            },
        );
        Ok(())
    }

    fn parse_type_sampler(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(2)?;
        let id = self.next()?;
        let decor = self.future_decor.remove(&id).unwrap_or_default();
        let handle = module.types.insert(
            crate::Type {
                name: decor.name,
                inner: crate::TypeInner::Sampler { comparison: false },
            },
            self.span_from_with_op(start),
        );
        self.lookup_type.insert(
            id,
            LookupType {
                handle,
                base_id: None,
            },
        );
        Ok(())
    }

    fn parse_constant(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect_at_least(4)?;
        let type_id = self.next()?;
        let id = self.next()?;
        let type_lookup = self.lookup_type.lookup(type_id)?;
        let ty = type_lookup.handle;

        let literal = match module.types[ty].inner {
            crate::TypeInner::Scalar(crate::Scalar {
                kind: crate::ScalarKind::Uint,
                width,
            }) => {
                let low = self.next()?;
                match width {
                    4 => crate::Literal::U32(low),
                    8 => {
                        inst.expect(5)?;
                        let high = self.next()?;
                        crate::Literal::U64((u64::from(high) << 32) | u64::from(low))
                    }
                    _ => return Err(Error::InvalidTypeWidth(width as u32)),
                }
            }
            crate::TypeInner::Scalar(crate::Scalar {
                kind: crate::ScalarKind::Sint,
                width,
            }) => {
                let low = self.next()?;
                match width {
                    4 => crate::Literal::I32(low as i32),
                    8 => {
                        inst.expect(5)?;
                        let high = self.next()?;
                        crate::Literal::I64(((u64::from(high) << 32) | u64::from(low)) as i64)
                    }
                    _ => return Err(Error::InvalidTypeWidth(width as u32)),
                }
            }
            crate::TypeInner::Scalar(crate::Scalar {
                kind: crate::ScalarKind::Float,
                width,
            }) => {
                let low = self.next()?;
                match width {
                    // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#Literal
                    // If a numeric type’s bit width is less than 32-bits, the value appears in the low-order bits of the word.
                    2 => crate::Literal::F16(f16::from_bits(low as u16)),
                    4 => crate::Literal::F32(f32::from_bits(low)),
                    8 => {
                        inst.expect(5)?;
                        let high = self.next()?;
                        crate::Literal::F64(f64::from_bits(
                            (u64::from(high) << 32) | u64::from(low),
                        ))
                    }
                    _ => return Err(Error::InvalidTypeWidth(width as u32)),
                }
            }
            _ => return Err(Error::UnsupportedType(type_lookup.handle)),
        };

        let span = self.span_from_with_op(start);

        let init = module
            .global_expressions
            .append(crate::Expression::Literal(literal), span);

        self.insert_parsed_constant(module, id, type_id, ty, init, span)
    }

    fn parse_composite_constant(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect_at_least(3)?;
        let type_id = self.next()?;
        let id = self.next()?;

        let type_lookup = self.lookup_type.lookup(type_id)?;
        let ty = type_lookup.handle;

        let mut components = Vec::with_capacity(inst.wc as usize - 3);
        for _ in 0..components.capacity() {
            let start = self.data_offset;
            let component_id = self.next()?;
            let span = self.span_from_with_op(start);
            let constant = self.lookup_constant.lookup(component_id)?;
            let expr = module
                .global_expressions
                .append(constant.inner.to_expr(), span);
            components.push(expr);
        }

        let span = self.span_from_with_op(start);

        let init = module
            .global_expressions
            .append(crate::Expression::Compose { ty, components }, span);

        self.insert_parsed_constant(module, id, type_id, ty, init, span)
    }

    fn parse_null_constant(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(3)?;
        let type_id = self.next()?;
        let id = self.next()?;
        let span = self.span_from_with_op(start);

        let type_lookup = self.lookup_type.lookup(type_id)?;
        let ty = type_lookup.handle;

        let init = module
            .global_expressions
            .append(crate::Expression::ZeroValue(ty), span);

        self.insert_parsed_constant(module, id, type_id, ty, init, span)
    }

    fn parse_bool_constant(
        &mut self,
        inst: Instruction,
        value: bool,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect(3)?;
        let type_id = self.next()?;
        let id = self.next()?;
        let span = self.span_from_with_op(start);

        let type_lookup = self.lookup_type.lookup(type_id)?;
        let ty = type_lookup.handle;

        let init = module.global_expressions.append(
            crate::Expression::Literal(crate::Literal::Bool(value)),
            span,
        );

        self.insert_parsed_constant(module, id, type_id, ty, init, span)
    }

    fn insert_parsed_constant(
        &mut self,
        module: &mut crate::Module,
        id: u32,
        type_id: u32,
        ty: Handle<crate::Type>,
        init: Handle<crate::Expression>,
        span: crate::Span,
    ) -> Result<(), Error> {
        let decor = self.future_decor.remove(&id).unwrap_or_default();

        let inner = if let Some(id) = decor.specialization_constant_id {
            let o = crate::Override {
                name: decor.name,
                id: Some(id.try_into().map_err(|_| Error::SpecIdTooHigh(id))?),
                ty,
                init: Some(init),
            };
            Constant::Override(module.overrides.append(o, span))
        } else {
            let c = crate::Constant {
                name: decor.name,
                ty,
                init,
            };
            Constant::Constant(module.constants.append(c, span))
        };

        self.lookup_constant
            .insert(id, LookupConstant { inner, type_id });
        Ok(())
    }

    fn parse_global_variable(
        &mut self,
        inst: Instruction,
        module: &mut crate::Module,
    ) -> Result<(), Error> {
        let start = self.data_offset;
        self.switch(ModuleState::Type, inst.op)?;
        inst.expect_at_least(4)?;
        let type_id = self.next()?;
        let id = self.next()?;
        let storage_class = self.next()?;
        let init = if inst.wc > 4 {
            inst.expect(5)?;
            let start = self.data_offset;
            let init_id = self.next()?;
            let span = self.span_from_with_op(start);
            let lconst = self.lookup_constant.lookup(init_id)?;
            let expr = module
                .global_expressions
                .append(lconst.inner.to_expr(), span);
            Some(expr)
        } else {
            None
        };
        let span = self.span_from_with_op(start);
        let dec = self.future_decor.remove(&id).unwrap_or_default();

        let original_ty = self.lookup_type.lookup(type_id)?.handle;
        let mut ty = original_ty;

        if let crate::TypeInner::Pointer { base, space: _ } = module.types[original_ty].inner {
            ty = base;
        }

        if let crate::TypeInner::BindingArray { .. } = module.types[original_ty].inner {
            // Inside `parse_type_array()` we guess that an array of images or
            // samplers must be a binding array, and here we validate that guess
            if dec.desc_set.is_none() || dec.desc_index.is_none() {
                return Err(Error::NonBindingArrayOfImageOrSamplers);
            }
        }

        if let crate::TypeInner::Image {
            dim,
            arrayed,
            class: crate::ImageClass::Storage { format, access: _ },
        } = module.types[ty].inner
        {
            // Storage image types in IR have to contain the access, but not in the SPIR-V.
            // The same image type in SPIR-V can be used (and has to be used) for multiple images.
            // So we copy the type out and apply the variable access decorations.
            let access = dec.flags.to_storage_access();

            ty = module.types.insert(
                crate::Type {
                    name: None,
                    inner: crate::TypeInner::Image {
                        dim,
                        arrayed,
                        class: crate::ImageClass::Storage { format, access },
                    },
                },
                Default::default(),
            );
        }

        let ext_class = match self.lookup_storage_buffer_types.get(&ty) {
            Some(&access) => ExtendedClass::Global(crate::AddressSpace::Storage { access }),
            None => map_storage_class(storage_class)?,
        };

        let (inner, var) = match ext_class {
            ExtendedClass::Global(mut space) => {
                if let crate::AddressSpace::Storage { ref mut access } = space {
                    *access &= dec.flags.to_storage_access();
                }
                let var = crate::GlobalVariable {
                    binding: dec.resource_binding(),
                    name: dec.name,
                    space,
                    ty,
                    init,
                };
                (Variable::Global, var)
            }
            ExtendedClass::Input => {
                let binding = dec.io_binding()?;
                let mut unsigned_ty = ty;
                if let crate::Binding::BuiltIn(built_in) = binding {
                    let needs_inner_uint = match built_in {
                        crate::BuiltIn::BaseInstance
                        | crate::BuiltIn::BaseVertex
                        | crate::BuiltIn::InstanceIndex
                        | crate::BuiltIn::SampleIndex
                        | crate::BuiltIn::VertexIndex
                        | crate::BuiltIn::PrimitiveIndex
                        | crate::BuiltIn::LocalInvocationIndex => {
                            Some(crate::TypeInner::Scalar(crate::Scalar::U32))
                        }
                        crate::BuiltIn::GlobalInvocationId
                        | crate::BuiltIn::LocalInvocationId
                        | crate::BuiltIn::WorkGroupId
                        | crate::BuiltIn::WorkGroupSize => Some(crate::TypeInner::Vector {
                            size: crate::VectorSize::Tri,
                            scalar: crate::Scalar::U32,
                        }),
                        _ => None,
                    };
                    if let (Some(inner), Some(crate::ScalarKind::Sint)) =
                        (needs_inner_uint, module.types[ty].inner.scalar_kind())
                    {
                        unsigned_ty = module
                            .types
                            .insert(crate::Type { name: None, inner }, Default::default());
                    }
                }

                let var = crate::GlobalVariable {
                    name: dec.name.clone(),
                    space: crate::AddressSpace::Private,
                    binding: None,
                    ty,
                    init: None,
                };

                let inner = Variable::Input(crate::FunctionArgument {
                    name: dec.name,
                    ty: unsigned_ty,
                    binding: Some(binding),
                });
                (inner, var)
            }
            ExtendedClass::Output => {
                // For output interface blocks, this would be a structure.
                let binding = dec.io_binding().ok();
                let init = match binding {
                    Some(crate::Binding::BuiltIn(built_in)) => {
                        match null::generate_default_built_in(
                            Some(built_in),
                            ty,
                            &mut module.global_expressions,
                            span,
                        ) {
                            Ok(handle) => Some(handle),
                            Err(e) => {
                                log::warn!("Failed to initialize output built-in: {}", e);
                                None
                            }
                        }
                    }
                    Some(crate::Binding::Location { .. }) => None,
                    None => match module.types[ty].inner {
                        crate::TypeInner::Struct { ref members, .. } => {
                            let mut components = Vec::with_capacity(members.len());
                            for member in members.iter() {
                                let built_in = match member.binding {
                                    Some(crate::Binding::BuiltIn(built_in)) => Some(built_in),
                                    _ => None,
                                };
                                let handle = null::generate_default_built_in(
                                    built_in,
                                    member.ty,
                                    &mut module.global_expressions,
                                    span,
                                )?;
                                components.push(handle);
                            }
                            Some(
                                module
                                    .global_expressions
                                    .append(crate::Expression::Compose { ty, components }, span),
                            )
                        }
                        _ => None,
                    },
                };

                let var = crate::GlobalVariable {
                    name: dec.name,
                    space: crate::AddressSpace::Private,
                    binding: None,
                    ty,
                    init,
                };
                let inner = Variable::Output(crate::FunctionResult { ty, binding });
                (inner, var)
            }
        };

        let handle = module.global_variables.append(var, span);

        if module.types[ty].inner.can_comparison_sample(module) {
            log::debug!("\t\ttracking {:?} for sampling properties", handle);

            self.handle_sampling
                .insert(handle, image::SamplingFlags::empty());
        }

        self.lookup_variable.insert(
            id,
            LookupVariable {
                inner,
                handle,
                type_id,
            },
        );
        Ok(())
    }

    /// Record an atomic access to some component of a global variable.
    ///
    /// Given `handle`, an expression referring to a scalar that has had an
    /// atomic operation applied to it, descend into the expression, noting
    /// which global variable it ultimately refers to, and which struct fields
    /// of that global's value it accesses.
    ///
    /// Return the handle of the type of the expression.
    ///
    /// If the expression doesn't actually refer to something in a global
    /// variable, we can't upgrade its type in a way that Naga validation would
    /// pass, so reject the input instead.
    fn record_atomic_access(
        &mut self,
        ctx: &BlockContext,
        handle: Handle<crate::Expression>,
    ) -> Result<Handle<crate::Type>, Error> {
        log::debug!("\t\tlocating global variable in {handle:?}");
        match ctx.expressions[handle] {
            crate::Expression::Access { base, index } => {
                log::debug!("\t\t  access {handle:?} {index:?}");
                let ty = self.record_atomic_access(ctx, base)?;
                let crate::TypeInner::Array { base, .. } = ctx.module.types[ty].inner else {
                    unreachable!("Atomic operations on Access expressions only work for arrays");
                };
                Ok(base)
            }
            crate::Expression::AccessIndex { base, index } => {
                log::debug!("\t\t  access index {handle:?} {index:?}");
                let ty = self.record_atomic_access(ctx, base)?;
                match ctx.module.types[ty].inner {
                    crate::TypeInner::Struct { ref members, .. } => {
                        let index = index as usize;
                        self.upgrade_atomics.insert_field(ty, index);
                        Ok(members[index].ty)
                    }
                    crate::TypeInner::Array { base, .. } => {
                        Ok(base)
                    }
                    _ => unreachable!("Atomic operations on AccessIndex expressions only work for structs and arrays"),
                }
            }
            crate::Expression::GlobalVariable(h) => {
                log::debug!("\t\t  found {h:?}");
                self.upgrade_atomics.insert_global(h);
                Ok(ctx.module.global_variables[h].ty)
            }
            _ => Err(Error::AtomicUpgradeError(
                crate::front::atomic_upgrade::Error::GlobalVariableMissing,
            )),
        }
    }
}

fn make_index_literal(
    ctx: &mut BlockContext,
    index: u32,
    block: &mut crate::Block,
    emitter: &mut crate::proc::Emitter,
    index_type: Handle<crate::Type>,
    index_type_id: spirv::Word,
    span: crate::Span,
) -> Result<Handle<crate::Expression>, Error> {
    block.extend(emitter.finish(ctx.expressions));

    let literal = match ctx.module.types[index_type].inner.scalar_kind() {
        Some(crate::ScalarKind::Uint) => crate::Literal::U32(index),
        Some(crate::ScalarKind::Sint) => crate::Literal::I32(index as i32),
        _ => return Err(Error::InvalidIndexType(index_type_id)),
    };
    let expr = ctx
        .expressions
        .append(crate::Expression::Literal(literal), span);

    emitter.start(ctx.expressions);
    Ok(expr)
}

fn resolve_constant(gctx: crate::proc::GlobalCtx, constant: &Constant) -> Option<u32> {
    let constant = match *constant {
        Constant::Constant(constant) => constant,
        Constant::Override(_) => return None,
    };
    match gctx.global_expressions[gctx.constants[constant].init] {
        crate::Expression::Literal(crate::Literal::U32(id)) => Some(id),
        crate::Expression::Literal(crate::Literal::I32(id)) => Some(id as u32),
        _ => None,
    }
}

pub fn parse_u8_slice(data: &[u8], options: &Options) -> Result<crate::Module, Error> {
    if data.len() % 4 != 0 {
        return Err(Error::IncompleteData);
    }

    let words = data
        .chunks(4)
        .map(|c| u32::from_le_bytes(c.try_into().unwrap()));
    Frontend::new(words, options).parse()
}

/// Helper function to check if `child` is in the scope of `parent`
fn is_parent(mut child: usize, parent: usize, block_ctx: &BlockContext) -> bool {
    loop {
        if child == parent {
            // The child is in the scope parent
            break true;
        } else if child == 0 {
            // Searched finished at the root the child isn't in the parent's body
            break false;
        }

        child = block_ctx.bodies[child].parent;
    }
}

#[cfg(test)]
mod test {
    use alloc::vec;

    #[test]
    fn parse() {
        let bin = vec![
            // Magic number.           Version number: 1.0.
            0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00,
            // Generator number: 0.    Bound: 0.
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Reserved word: 0.
            0x00, 0x00, 0x00, 0x00, // OpMemoryModel.          Logical.
            0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, // GLSL450.
            0x01, 0x00, 0x00, 0x00,
        ];
        let _ = super::parse_u8_slice(&bin, &Default::default()).unwrap();
    }
}
