/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Business logic for the crash reporter.

use crate::std::{
    cell::RefCell,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering::Relaxed},
        Arc, Mutex, Weak,
    },
};
use crate::{
    async_task::AsyncTask,
    config::Config,
    memory_test::child::Memtest,
    net,
    settings::Settings,
    std,
    ui::{ReportCrashUI, ReportCrashUIState, SubmitState},
};
use anyhow::Context;
use uuid::Uuid;

pub mod annotations;

/// The main crash reporting logic.
pub struct ReportCrash {
    pub settings: RefCell<Settings>,
    config: Arc<Config>,
    extra: serde_json::Value,
    settings_file: PathBuf,
    attempted_to_send: AtomicBool,
    ui: Option<Arc<AsyncTask<ReportCrashUIState>>>,
    memtest: RefCell<Option<Memtest>>,
}

fn modify_extra_for_report(extra: &mut serde_json::Value) {
    if let Some(map) = extra.as_object_mut() {
        map.retain(|k, _| annotations::send_in_report(k));
    }

    extra["SubmittedFrom"] = "Client".into();
    extra["Throttleable"] = "1".into();
}

impl ReportCrash {
    pub fn new(config: Arc<Config>, extra: serde_json::Value) -> anyhow::Result<Self> {
        let settings_file = config.data_dir().join("crashreporter_settings.json");
        let settings: Settings = match std::fs::File::open(&settings_file) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
                anyhow::bail!(
                    "failed to open settings file ({}): {e}",
                    settings_file.display()
                );
            }
            Err(_) => Default::default(),
            Ok(f) => Settings::from_reader(f)?,
        };

        Ok(ReportCrash {
            config,
            extra,
            settings_file,
            settings: settings.into(),
            attempted_to_send: Default::default(),
            ui: None,
            memtest: None.into(),
        })
    }

    /// Returns whether an attempt was made to send the report.
    pub fn run(mut self) -> anyhow::Result<bool> {
        self.memtest_according_to_settings();
        self.set_log_file();
        let hash = self.compute_minidump_hash().map(Some).unwrap_or_else(|e| {
            log::warn!("failed to compute minidump hash: {e:#}");
            None
        });
        let ping_uuid = self.send_crash_ping(hash.as_deref());
        if let Err(e) = self.update_events_file(hash.as_deref(), ping_uuid) {
            log::warn!("failed to update events file: {e:#}");
        }
        self.check_eol_version()?;

        if !self.config.auto_submit {
            self.run_ui();
        } else {
            anyhow::ensure!(self.try_send().unwrap_or(false), "failed to send report");
        }

        Ok(self.attempted_to_send.load(Relaxed))
    }

    /// Set the log file based on the current configured paths.
    ///
    /// This is the earliest that this can occur as the configuration data dir may be set based on
    /// fields in the extra file.
    fn set_log_file(&self) {
        if let Some(log_target) = &self.config.log_target {
            log_target.set_file(&self.config.data_dir().join("submit.log"));
        }
    }

    /// Compute the SHA256 hash of the minidump file contents, and return it as a hex string.
    fn compute_minidump_hash(&self) -> anyhow::Result<String> {
        let hash = {
            use sha2::{Digest, Sha256};
            let mut dump_file = std::fs::File::open(self.config.dump_file())?;
            let mut hasher = Sha256::new();
            std::io::copy(&mut dump_file, &mut hasher)?;
            hasher.finalize()
        };

        let mut s = String::with_capacity(hash.len() * 2);
        for byte in hash {
            use crate::std::fmt::Write;
            write!(s, "{:02x}", byte).unwrap();
        }

        Ok(s)
    }

    /// Send crash pings to legacy telemetry and Glean.
    ///
    /// Returns the crash ping uuid used in legacy telemetry.
    fn send_crash_ping(&self, minidump_hash: Option<&str>) -> Option<Uuid> {
        net::ping::CrashPing {
            crash_id: self.config.local_dump_id().as_ref(),
            extra: &self.extra,
            ping_dir: self.config.ping_dir.as_deref(),
            minidump_hash,
            pingsender_path: crate::config::installation_program_path("pingsender").as_ref(),
        }
        .send()
    }

    /// Update the events file with information about the crash ping, minidump hash, and
    /// stacktraces.
    fn update_events_file(
        &self,
        minidump_hash: Option<&str>,
        ping_uuid: Option<Uuid>,
    ) -> anyhow::Result<()> {
        use crate::std::io::{BufRead, Error, ErrorKind, Write};
        struct EventsFile {
            event_version: String,
            time: String,
            uuid: String,
            pub data: serde_json::Value,
        }

        impl EventsFile {
            pub fn parse(mut reader: impl BufRead) -> std::io::Result<Self> {
                let mut lines = (&mut reader).lines();

                let mut read_field = move |name: &str| -> std::io::Result<String> {
                    lines.next().transpose()?.ok_or_else(|| {
                        Error::new(ErrorKind::InvalidData, format!("missing {name} field"))
                    })
                };

                let event_version = read_field("event version")?;
                let time = read_field("time")?;
                let uuid = read_field("uuid")?;
                let data = serde_json::from_reader(reader)?;
                Ok(EventsFile {
                    event_version,
                    time,
                    uuid,
                    data,
                })
            }

            pub fn write(&self, mut writer: impl Write) -> std::io::Result<()> {
                writeln!(writer, "{}", self.event_version)?;
                writeln!(writer, "{}", self.time)?;
                writeln!(writer, "{}", self.uuid)?;
                serde_json::to_writer(writer, &self.data)?;
                Ok(())
            }
        }

        let Some(events_dir) = &self.config.events_dir else {
            log::warn!("not updating the events file; no events directory configured");
            return Ok(());
        };

        let event_path = events_dir.join(self.config.local_dump_id().as_ref());

        // Read events file.
        let file = std::fs::File::open(&event_path)
            .with_context(|| format!("failed to open event file at {}", event_path.display()))?;

        let mut events_file =
            EventsFile::parse(std::io::BufReader::new(file)).with_context(|| {
                format!(
                    "failed to parse events file contents in {}",
                    event_path.display()
                )
            })?;

        // Update events file fields.
        if let Some(hash) = minidump_hash {
            events_file.data["MinidumpSha256Hash"] = hash.into();
        }
        if let Some(uuid) = ping_uuid {
            events_file.data["CrashPingUUID"] = uuid.to_string().into();
        }
        events_file.data["StackTraces"] = self.extra["StackTraces"].clone();

        // Write altered events file.
        let file = std::fs::File::create(&event_path).with_context(|| {
            format!("failed to truncate event file at {}", event_path.display())
        })?;

        events_file
            .write(file)
            .with_context(|| format!("failed to write event file at {}", event_path.display()))
    }

    /// Check whether the version of the software that generated the crash is EOL.
    fn check_eol_version(&self) -> anyhow::Result<()> {
        if let Some(version) = self.extra["Version"].as_str() {
            if self.config.version_eol_file(version).exists() {
                self.config.delete_files();
                anyhow::bail!(self.config.string("crashreporter-error-version-eol"));
            }
        }
        Ok(())
    }

    /// Save the current settings.
    fn save_settings(&self) {
        let result: anyhow::Result<()> = (|| {
            Ok(self
                .settings
                .borrow()
                .to_writer(std::fs::File::create(&self.settings_file)?)?)
        })();
        if let Err(e) = result {
            log::error!("error while saving settings: {e}");
        }
    }

    /// Handle a response from submitting a crash report.
    ///
    /// Returns the crash ID to use for the recorded submission event. Errors in this function may
    /// result in None being returned to consider the crash report submission as a failure even
    /// though the server did provide a response.
    fn handle_crash_report_response(
        &self,
        response: net::report::Response,
    ) -> anyhow::Result<Option<String>> {
        if let Some(version) = response.stop_sending_reports_for {
            // Create the EOL version file. The content seemingly doesn't matter, but we mimic what
            // was written by the old crash reporter.
            if let Err(e) = std::fs::write(self.config.version_eol_file(&version), "1\n") {
                log::warn!("failed to write EOL file: {e}");
            }
        }

        if response.discarded {
            log::debug!("response indicated that the report was discarded");
            return Ok(None);
        }

        let Some(crash_id) = response.crash_id else {
            log::debug!("response did not provide a crash id");
            return Ok(None);
        };

        // Write the id to the `submitted` directory
        let submitted_dir = self.config.submitted_crash_dir();
        std::fs::create_dir_all(&submitted_dir).with_context(|| {
            format!(
                "failed to create submitted crash directory {}",
                submitted_dir.display()
            )
        })?;

        let crash_id_file = submitted_dir.join(format!("{crash_id}.txt"));

        let mut file = std::fs::File::create(&crash_id_file).with_context(|| {
            format!(
                "failed to create submitted crash file for {crash_id} ({})",
                crash_id_file.display()
            )
        })?;

        // Shadow `std::fmt::Write` to use the correct trait below.
        use crate::std::io::Write;

        if let Err(e) = writeln!(
            &mut file,
            "{}",
            self.config
                .build_string("crashreporter-crash-identifier")
                .arg("id", &crash_id)
                .get()
        ) {
            log::warn!(
                "failed to write to submitted crash file ({}) for {crash_id}: {e}",
                crash_id_file.display()
            );
        }

        if let Some(url) = response.view_url {
            if let Err(e) = writeln!(
                &mut file,
                "{}",
                self.config
                    .build_string("crashreporter-crash-details")
                    .arg("url", url)
                    .get()
            ) {
                log::warn!(
                    "failed to write view url to submitted crash file ({}) for {crash_id}: {e}",
                    crash_id_file.display()
                );
            }
        }

        Ok(Some(crash_id))
    }

    /// Write the submission event.
    ///
    /// A `None` crash_id indicates that the submission failed.
    fn write_submission_event(&self, crash_id: Option<String>) -> anyhow::Result<()> {
        let Some(events_dir) = &self.config.events_dir else {
            // If there's no events dir, don't do anything.
            return Ok(());
        };

        let local_id = self.config.local_dump_id();
        let event_path = events_dir.join(format!("{local_id}-submission"));

        let unix_epoch_seconds = std::time::SystemTime::now()
            .duration_since(std::time::SystemTime::UNIX_EPOCH)
            .expect("system time is before the unix epoch")
            .as_secs();
        std::fs::write(
            &event_path,
            format!(
                "crash.submission.1\n{unix_epoch_seconds}\n{local_id}\n{}\n{}",
                crash_id.is_some(),
                crash_id.as_deref().unwrap_or("")
            ),
        )
        .with_context(|| format!("failed to write event to {}", event_path.display()))
    }

    /// Restart the program.
    fn restart_process(&self) {
        self.config.restart_process();
    }

    /// Run the crash reporting UI.
    fn run_ui(&mut self) {
        use crate::std::{sync::mpsc, thread};

        let (logic_send, logic_recv) = mpsc::channel();
        // Wrap work_send in an Arc so that it can be captured weakly by the work queue and
        // drop when the UI finishes, including panics (allowing the logic thread to exit).
        //
        // We need to wrap in a Mutex because std::mpsc::Sender isn't Sync (until rust 1.72).
        let logic_send = Arc::new(Mutex::new(logic_send));

        let weak_logic_send = Arc::downgrade(&logic_send);
        let logic_remote_queue = AsyncTask::new(move |f| {
            if let Some(logic_send) = weak_logic_send.upgrade() {
                // This is best-effort: ignore errors.
                let _ = logic_send.lock().unwrap().send(f);
            }
        });

        let crash_ui = ReportCrashUI::new(
            &*self.settings.borrow(),
            self.config.clone(),
            logic_remote_queue,
        );

        // Set the UI remote queue.
        let crash_ui_async_task = Arc::new(crash_ui.async_task());
        struct PanicHandler(Weak<AsyncTask<ReportCrashUIState>>);
        impl Drop for PanicHandler {
            fn drop(&mut self) {
                if let Some(ui) = self.0.upgrade() {
                    ui.push(|_| panic!("logic thread panicked"));
                }
            }
        }
        let logic_panic_handler = PanicHandler(Arc::downgrade(&crash_ui_async_task));
        self.ui = Some(crash_ui_async_task);

        // Spawn a separate thread to handle all interactions with `self`. This prevents blocking
        // the UI for any reason.

        // Use a barrier to ensure both threads are live before either starts (ensuring they
        // can immediately queue work for each other).
        let barrier = std::sync::Barrier::new(2);
        let barrier = &barrier;
        thread::scope(move |s| {
            // Move `logic_send` into this scope so that it will drop when the scope completes
            // (which will drop the `mpsc::Sender` and cause the logic thread to complete and join
            // when the UI finishes so the scope can exit).
            let _logic_send = logic_send;
            s.spawn(move || {
                let _logic_panic_handler = logic_panic_handler;
                barrier.wait();
                while let Ok(f) = logic_recv.recv() {
                    f(self);
                }
                // Save settings after UI is closed
                self.save_settings();

                // Clear the UI remote queue, using it after this point is an error. This also
                // prevents the panic handler from engaging.
                //
                // NOTE we do this here because the compiler can't reason about `self` being safely
                // accessible after `thread::scope` returns. This is effectively the same result
                // since the above loop will only exit when `logic_send` is dropped at the end of
                // the scope.
                self.ui = None;

                // Save settings after UI is closed
                self.save_settings();
            });

            barrier.wait();
            crash_ui.run()
        });
    }

    /// Overwrite the extra file with the given content.
    fn update_extra_file(&self, content: &serde_json::Value) -> anyhow::Result<()> {
        let Some(path) = self.config.extra_file() else {
            return Ok(());
        };
        let f = std::fs::File::create(&path)
            .with_context(|| format!("failed to truncate {}", path.display().to_string()))?;
        serde_json::to_writer(f, content)
            .with_context(|| format!("failed to overwrite {}", path.display().to_string()))
    }

    /// Start or stop a memtest if settings dictate it.
    fn memtest_according_to_settings(&self) {
        let memtest_enabled = self.config.run_memtest && self.settings.borrow().test_hardware;
        let mut memtest = self.memtest.borrow_mut();
        if memtest_enabled && memtest.is_none() {
            *memtest = Memtest::spawn();
        } else if !memtest_enabled && memtest.is_some() {
            *memtest = None;
        }
    }
}

// These methods may interact with `self.ui`.
impl ReportCrash {
    /// Update the submission details shown in the UI.
    pub fn update_details(&self) {
        use crate::std::fmt::Write;

        let mut extra = self.current_extra_data();
        modify_extra_for_report(&mut extra);

        let mut details = String::new();
        let mut entries: Vec<_> = extra.as_object().unwrap().into_iter().collect();
        entries.sort_unstable_by_key(|(k, _)| *k);
        for (key, value) in entries {
            let _ = write!(details, "{key}: ");
            if let Some(v) = value.as_str() {
                details.push_str(v);
            } else {
                match serde_json::to_string(value) {
                    Ok(s) => details.push_str(&s),
                    Err(e) => {
                        let _ = write!(details, "<serialization error: {e}>");
                    }
                }
            }
            let _ = writeln!(details);
        }
        let _ = writeln!(
            details,
            "{}",
            self.config.string("crashreporter-report-info")
        );

        self.ui().push(move |ui| *ui.details.borrow_mut() = details);
    }

    /// Restart the application and send the crash report.
    pub fn restart(&self) {
        // Get the program restarted before sending the report.
        self.restart_process();
        let result = self.try_send();
        self.close_window(result.is_some());
    }

    /// Quit and send the crash report.
    pub fn quit(&self) {
        let result = self.try_send();
        self.close_window(result.is_some());
    }

    fn close_window(&self, report_sent: bool) {
        if report_sent && !self.config.auto_submit && !cfg!(test) {
            // Add a delay to allow the user to see the result.
            std::thread::sleep(std::time::Duration::from_secs(5));
        }

        self.ui().push(|r| r.close_window.fire(&()));
    }

    /// Try to send the report.
    ///
    /// This function may be called without a UI active (if auto_submit is true), so it will not
    /// panic if `self.ui` is unset.
    ///
    /// Returns whether the report was received (regardless of whether the response was processed
    /// successfully), if a report could be sent at all (based on the configuration).
    fn try_send(&self) -> Option<bool> {
        // Whether the user wants to submit the report or not, we record that we attempted a send
        // (so to speak), confirming that we got to the point of user input. This will retain the
        // crash files rather than deleting them. E.g., the user may want to submit it later through
        // `about:crashes`.
        self.attempted_to_send.store(true, Relaxed);

        let send_report = self.settings.borrow().submit_report;

        if !send_report {
            log::trace!("not sending report due to user setting");
            return None;
        }

        // Potentially start/stop a memtest now, in case the settings have changed since launch.
        self.memtest_according_to_settings();

        // Incorporate user input into the extra data (which is acknowledged by "submit report"
        // being enabled).
        let mut extra = self.current_extra_data();

        // Store memtest output. The previous `memtest_according_to_settings()` ensures that we
        // only add the output if the setting is enabled.
        if let Some(memtest) = self.memtest.borrow_mut().take() {
            if let Some(ui) = &self.ui {
                ui.push(|r| *r.submit_state.borrow_mut() = SubmitState::WaitingHardwareTests);
            }

            match memtest.collect_output_for_submission() {
                Err(e) => log::error!("couldn't get memtest output: {e:#}"),
                Ok(s) => extra["MemtestOutput"] = s.into(),
            }
        }

        if let Some(ui) = &self.ui {
            ui.push(|r| *r.submit_state.borrow_mut() = SubmitState::InProgress);
        }

        // Update the extra file, since we may not be deleting it per the `attempted_to_send`
        // update, and it now matches exactly what we'll be sending.
        if let Err(e) = self.update_extra_file(&extra) {
            log::error!("failed to update extra file: {e:#}");
            // We can proceed in the event of an error; this is best-effort and serves as
            // insurance in case submission fails.
        }

        modify_extra_for_report(&mut extra);

        // The extra contents cannot change beyond this point.
        let extra = extra;

        // TODO? load proxy info from libgconf on linux

        let Some(url) = &self.config.report_url else {
            log::warn!("not sending report due to missing report url");
            return None;
        };

        let Some(url) = url.to_str() else {
            log::warn!(
                "not sending report due to provided url containing invalid utf8 characters: {url:?}"
            );
            return None;
        };

        // Send the report to the server.
        let memory_file = self.config.memory_file();
        let report = net::report::CrashReport {
            extra: &extra,
            dump_file: self.config.dump_file(),
            memory_file: memory_file.as_deref(),
            url,
        };

        // Normally we might want to do the following asynchronously since it will block,
        // however we don't really need the Logic thread to do anything else (the UI
        // becomes disabled from this point onward), so we just do it here. Same goes for
        // the `std::thread::sleep` in close_window() later on.
        let report_response = report.send().map(Some).unwrap_or_else(|e| {
            log::error!("failed to send report: {e:#}");
            None
        });

        let report_received = report_response.is_some();
        let crash_id = report_response.and_then(|response| {
            self.handle_crash_report_response(response)
                .unwrap_or_else(|e| {
                    log::error!("failed to handle crash report response: {e}");
                    None
                })
        });

        if report_received {
            // If the response could be handled (indicated by the returned crash id), clean up by
            // deleting the minidump files. Otherwise, prune old minidump files.
            if crash_id.is_some() {
                self.config.delete_files();
            } else {
                if let Err(e) = self.config.prune_files() {
                    log::warn!("failed to prune files: {e}");
                }
            }
        }

        if let Err(e) = self.write_submission_event(crash_id) {
            log::warn!("failed to write submission event: {e}");
        }

        // Indicate whether the report was sent successfully, regardless of whether the response
        // was processed successfully.
        //
        // FIXME: this is how the old crash reporter worked, but we might want to change this
        // behavior.
        if let Some(ui) = &self.ui {
            ui.push(move |r| {
                *r.submit_state.borrow_mut() = if report_received {
                    SubmitState::Success
                } else {
                    SubmitState::Failure
                }
            });
        }

        Some(report_received)
    }

    /// Form the extra data, taking into account user input.
    fn current_extra_data(&self) -> serde_json::Value {
        let include_address = self.settings.borrow().include_url;
        let comment = if !self.config.auto_submit {
            self.ui().wait(|r| r.comment.get())
        } else {
            Default::default()
        };

        let mut extra = self.extra.clone();

        if !comment.is_empty() {
            extra["Comments"] = comment.into();
        }

        if !include_address {
            extra.as_object_mut().unwrap().remove("URL");
        }

        extra
    }

    fn ui(&self) -> &AsyncTask<ReportCrashUIState> {
        self.ui.as_ref().expect("UI remote queue missing")
    }
}
