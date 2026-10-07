#![windows_subsystem = "windows"]
//! BlockBreach: the installer and launcher of the Ready or Not x Minecraft passthrough mod.

mod core;
mod i18n;
mod ui;

use std::path::PathBuf;
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Instant;

use eframe::egui::{self, pos2, vec2, Align2, Color32, CornerRadius, Pos2, Rect, Sense, Stroke, StrokeKind};

use crate::core::{LineKind, Play, PlayStep, Progress, State};
use crate::i18n::{Tr, EN, RU};
use crate::ui::*;

const W: f32 = 1080.0;
const H: f32 = 700.0;
/// the left menu's width; the pages start right of it
const SIDE: f32 = 300.0;

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Play,
    Install,
    Settings,
    Controls,
    About,
}

#[derive(Clone, Copy, PartialEq)]
enum JobKind {
    Install,
    Uninstall,
}

struct App {
    page: Page,
    state: State,
    progress: Progress,
    job: Option<(JobKind, JoinHandle<State>)>,
    last_job: Option<JobKind>,
    play: Play,
    play_thread: Option<JoinHandle<()>>,
    ron_text: String,
    mc_text: String,
    path_error: Option<String>,
    launcher: Option<core::Launcher>,
    refresh_at: Instant,
    ron_live: bool,
    mc_live: bool,
    logo: Option<egui::TextureHandle>,
    backdrop: Option<egui::TextureHandle>,
    saved_at: Option<Instant>,
    confirm_uninstall: bool,
    t0: Instant,
    splash: usize,
    exit_after_uninstall: bool,
    /// dev: --shot <page> <out.bmp> renders off-screen, saves one frame and quits
    shot: Option<(PathBuf, u32)>,
    /// --demo: a scripted run for the showcase video (fake pointer, real install, then READY)
    demo: Option<Instant>,
    demo_done: u32,
    job_started: Option<Instant>,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>, page: Page, confirm_uninstall: bool) -> App {
        install_fonts(&cc.egui_ctx);
        style(&cc.egui_ctx);
        let mut state = State::load();
        if state.settings.language.is_empty() {
            state.settings.language = if core::system_language_ru() { "ru".into() } else { "en".into() };
        }
        if state.ron_dir.as_ref().map(|d| !core::ron_valid(d)).unwrap_or(true) {
            state.ron_dir = core::find_ron();
        }
        if state.mc_dir.is_none() {
            state.mc_dir = Some(core::default_mc_dir());
        }
        let ron_text = state.ron_dir.as_ref().map(|p| pretty(p)).unwrap_or_default();
        let mc_text = state.mc_dir.as_ref().map(|p| pretty(p)).unwrap_or_default();
        let logo = egui::ColorImage::from_rgba_unmultiplied([128, 128], include_bytes!("../assets/logo_128.rgba"));
        let logo = Some(cc.egui_ctx.load_texture("logo", logo, egui::TextureOptions::LINEAR));
        let backdrop = image::load_from_memory(include_bytes!("../assets/bg.jpg")).ok().map(|i| {
            let i = i.to_rgba8();
            let size = [i.width() as usize, i.height() as usize];
            cc.egui_ctx.load_texture("backdrop", egui::ColorImage::from_rgba_unmultiplied(size, i.as_raw()), egui::TextureOptions::LINEAR)
        });
        App {
            page,
            state,
            progress: Progress::default(),
            job: None,
            last_job: None,
            play: Play::new(),
            play_thread: None,
            ron_text,
            mc_text,
            path_error: None,
            launcher: core::find_launcher(),
            refresh_at: Instant::now(),
            ron_live: false,
            mc_live: false,
            logo,
            backdrop,
            saved_at: None,
            confirm_uninstall,
            t0: Instant::now(),
            splash: (std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) % 6) as usize,
            exit_after_uninstall: false,
            shot: None,
            demo: None,
            demo_done: 0,
            job_started: None,
        }
    }

    fn tr(&self) -> &'static Tr {
        if self.state.settings.language == "ru" { &RU } else { &EN }
    }

    fn installed_ok(&self) -> Option<bool> {
        self.state.installed.as_ref().map(core::install_intact)
    }

    fn busy(&self) -> bool {
        self.job.is_some()
    }

    fn start_job(&mut self, kind: JobKind) {
        if self.busy() {
            return;
        }
        // the folders as typed
        let ron = PathBuf::from(self.ron_text.trim());
        if kind == JobKind::Install {
            if !core::ron_valid(&ron) {
                self.path_error = Some(self.tr().bad_ron_folder.into());
                return;
            }
            self.state.ron_dir = Some(ron);
            self.state.mc_dir = Some(PathBuf::from(self.mc_text.trim()));
        }
        self.path_error = None;
        self.state.save();
        self.progress.start();
        let job = core::Job { state: self.state.clone(), progress: self.progress.clone() };
        let handle = std::thread::spawn(move || match kind {
            JobKind::Install => core::install(job),
            JobKind::Uninstall => core::uninstall(job),
        });
        self.job = Some((kind, handle));
        self.job_started = Some(Instant::now());
        self.last_job = Some(kind);
    }

    fn poll_job(&mut self) {
        if let Some((kind, h)) = &self.job {
            if h.is_finished() {
                let kind = *kind;
                let (_, h) = self.job.take().unwrap();
                if let Ok(state) = h.join() {
                    self.state = state;
                }
                if kind == JobKind::Uninstall && self.state.installed.is_none() && core::running_from_app_dir() {
                    self.exit_after_uninstall = true;
                }
            }
        }
    }

    fn start_play(&mut self) {
        if self.play_thread.as_ref().map(|t| !t.is_finished()).unwrap_or(false) {
            return;
        }
        let state = self.state.clone();
        let play = self.play.clone();
        *play.0.lock().unwrap() = PlayStep::StartingLauncher;
        self.play_thread = Some(std::thread::spawn(move || core::play(state, play)));
    }
}

fn install_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let add = |fonts: &mut egui::FontDefinitions, name: &str, data: &'static [u8]| {
        fonts.font_data.insert(name.to_owned(), Arc::new(egui::FontData::from_static(data)));
    };
    add(&mut fonts, "oswald_b", include_bytes!("../assets/Oswald-Bold.ttf"));
    add(&mut fonts, "oswald_m", include_bytes!("../assets/Oswald-Medium.ttf"));
    add(&mut fonts, "roboto", include_bytes!("../assets/RobotoCond-Regular.ttf"));
    add(&mut fonts, "roboto_b", include_bytes!("../assets/RobotoCond-SemiBold.ttf"));
    add(&mut fonts, "jetbrains", include_bytes!("../assets/JetBrainsMono-Bold.ttf"));
    add(&mut fonts, "press", include_bytes!("../assets/PressStart2P-Regular.ttf"));
    let fallback: Vec<String> = fonts.families.get(&egui::FontFamily::Proportional).cloned().unwrap_or_default();
    let with = |first: &str| {
        let mut v = vec![first.to_owned()];
        v.extend(fallback.iter().cloned());
        v
    };
    for (family, font) in [("head", "oswald_b"), ("headm", "oswald_m"), ("body", "roboto"), ("bodyb", "roboto_b"), ("mono", "jetbrains"), ("pixel", "press")] {
        fonts.families.insert(egui::FontFamily::Name(family.into()), with(font));
    }
    fonts.families.insert(egui::FontFamily::Proportional, with("roboto"));
    ctx.set_fonts(fonts);
}

fn style(ctx: &egui::Context) {
    ctx.all_styles_mut(|s| {
        s.visuals = egui::Visuals::dark();
        s.visuals.extreme_bg_color = Color32::from_rgba_unmultiplied(0, 0, 0, 170);
        s.visuals.selection.bg_fill = with_alpha(BLUE, 0.45);
        s.visuals.selection.stroke = Stroke::new(1.0, BLUE);
        s.visuals.text_cursor.stroke = Stroke::new(2.0, Color32::WHITE);
        for w in [&mut s.visuals.widgets.inactive, &mut s.visuals.widgets.hovered, &mut s.visuals.widgets.active] {
            w.corner_radius = CornerRadius::ZERO;
        }
        s.visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, with_alpha(Color32::WHITE, 0.18));
        s.visuals.widgets.hovered.bg_stroke = Stroke::new(1.0, with_alpha(Color32::WHITE, 0.45));
        s.visuals.widgets.active.bg_stroke = Stroke::new(1.0, Color32::WHITE);
        s.visuals.override_text_color = Some(TEXT);
        s.text_styles.insert(egui::TextStyle::Body, body(16.0));
        s.text_styles.insert(egui::TextStyle::Monospace, mono(14.0));
        s.text_styles.insert(egui::TextStyle::Button, body(16.0));
    });
}

impl eframe::App for App {
    fn ui(&mut self, root: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = &root.ctx().clone();
        self.poll_job();
        if let Some((path, frames)) = &mut self.shot {
            *frames += 1;
            if *frames == 40 {
                ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(Default::default()));
            }
            let path = path.clone();
            let shots: Vec<_> = ctx.input(|i| i.raw.events.iter().filter_map(|e| if let egui::Event::Screenshot { image, .. } = e { Some(image.clone()) } else { None }).collect());
            if let Some(img) = shots.first() {
                save_bmp(&path, img);
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
        }
        if self.exit_after_uninstall && !self.busy() && self.progress.0.lock().unwrap().done.is_some() {
            // keep the result on screen for a moment, then go (the installed copy deletes itself)
            if self.refresh_at.elapsed().as_secs_f32() > 3.0 {
                core::schedule_self_delete();
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
        }
        if self.refresh_at.elapsed().as_secs_f32() > 1.0 {
            self.refresh_at = Instant::now();
            self.ron_live = core::ron_running();
            self.mc_live = core::minecraft_window().is_some();
            if self.launcher.is_none() {
                self.launcher = core::find_launcher();
            }
        }
        let t = self.t0.elapsed().as_secs_f32();
        egui::CentralPanel::default().frame(egui::Frame::NONE).show(root, |ui| {
            let full = ui.max_rect();
            let p = ui.painter().clone();
            background(&p, full, t, self.backdrop.as_ref());
            // the menu column: darker, one line on its right
            p.rect_filled(Rect::from_min_max(full.min, pos2(full.min.x + SIDE, full.max.y)), CornerRadius::ZERO, with_alpha(Color32::BLACK, 0.45));
            p.line_segment([pos2(full.min.x + SIDE, full.min.y + 36.0), pos2(full.min.x + SIDE, full.max.y)], Stroke::new(1.0, with_alpha(Color32::WHITE, 0.08)));
            ground_line(&p, Rect::from_min_max(pos2(full.min.x, full.max.y - 8.0), full.max));
            self.title_bar(ui, full);
            self.nav(ui, full);
            let content = Rect::from_min_max(pos2(full.min.x + SIDE + 36.0, full.min.y + 60.0), pos2(full.max.x - 32.0, full.max.y - 30.0));
            self.demo_step(ui);
            match self.page {
                Page::Play => self.page_play(ui, content, t),
                Page::Install => self.page_install(ui, content, t),
                Page::Settings => self.page_settings(ui, content),
                Page::Controls => self.page_controls(ui, content),
                Page::About => self.page_about(ui, content),
            }
        });
        if let Some((pos, down)) = VIRTUAL.with(|v| v.get()) {
            draw_pointer(&ctx.layer_painter(egui::LayerId::new(egui::Order::Tooltip, egui::Id::new("demo_pointer"))), pos, down);
        }
        ctx.request_repaint();
    }
}

/// A pixel arrow pointer with a click ripple.
fn draw_pointer(p: &egui::Painter, at: Pos2, down: bool) {
    const ARROW: [&str; 16] = [
        "X", "XX", "XoX", "XooX", "XoooX", "XooooX", "XoooooX", "XooooooX", "XoooooooX", "XooooooooX", "XoooooXXXX", "XooXooX", "XoX XooX",
        "XX  XooX", "X    XooX", "     XXX",
    ];
    let s = 2.0;
    if down {
        p.circle_stroke(at, 14.0, Stroke::new(3.0, with_alpha(AMBER, 0.8)));
    }
    for (y, row) in ARROW.iter().enumerate() {
        for (x, ch) in row.chars().enumerate() {
            let c = match ch {
                'X' => Color32::BLACK,
                'o' => Color32::WHITE,
                _ => continue,
            };
            p.rect_filled(Rect::from_min_size(at + vec2(x as f32 * s, y as f32 * s), vec2(s, s)), CornerRadius::ZERO, c);
        }
    }
}

impl App {
    /// The demo script: (time, pointer target) waypoints, clicks at given times.
    fn demo_step(&mut self, ui: &mut egui::Ui) {
        let Some(t0) = self.demo else { return };
        let now = Instant::now();
        let t = if now > t0 { (now - t0).as_secs_f32() } else { 0.0 };
        let o = ui.max_rect().min;
        // where the pointer is at each time (window coordinates); linear-ish easing between them
        let path: [(f32, Pos2); 8] = [
            (0.0, pos2(760.0, 640.0)),
            (1.4, pos2(760.0, 640.0)),
            (2.4, pos2(486.0, 444.0)),
            (9.4, pos2(486.0, 444.0)),
            (10.4, pos2(130.0, 193.0)),
            (11.0, pos2(130.0, 193.0)),
            (12.0, pos2(486.0, 444.0)),
            (40.0, pos2(486.0, 444.0)),
        ];
        let mut pos = path[0].1;
        for w in path.windows(2) {
            let ((ta, a), (tb, b)) = (w[0], w[1]);
            if t >= ta && t <= tb {
                let k = ((t - ta) / (tb - ta)).clamp(0.0, 1.0);
                let k = k * k * (3.0 - 2.0 * k);
                pos = a + (b - a) * k;
            }
        }
        if t > path[path.len() - 1].0 {
            pos = path[path.len() - 1].1;
        }
        let clicks = [2.6f32, 10.6, 12.2];
        let down = clicks.iter().any(|c| t >= *c && t < *c + 0.18);
        VIRTUAL.with(|v| v.set(Some((o + pos.to_vec2(), down))));
        // the actions, once each
        if self.demo_done == 0 && t >= 2.7 {
            self.demo_done = 1;
            self.start_job(JobKind::Install);
            self.page = Page::Install;
        }
        if self.demo_done == 1 && t >= 10.7 {
            self.demo_done = 2;
            self.page = Page::Play;
        }
        if self.demo_done == 2 && t >= 12.3 {
            self.demo_done = 3;
            if self.installed_ok() == Some(true) {
                self.start_play();
            }
        }
    }
}

impl App {
    fn title_bar(&mut self, ui: &mut egui::Ui, full: Rect) {
        let bar = Rect::from_min_size(full.min, vec2(full.width(), 36.0));
        let p = ui.painter().clone();
        p.rect_filled(bar, CornerRadius::ZERO, with_alpha(Color32::BLACK, 0.72));
        p.line_segment([bar.left_bottom(), bar.right_bottom()], Stroke::new(1.0, with_alpha(Color32::WHITE, 0.08)));
        let drag = ui.interact(bar, ui.id().with("titlebar"), Sense::click_and_drag());
        if drag.drag_started() {
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::StartDrag);
        }
        if let Some(logo) = &self.logo {
            p.image(logo.id(), Rect::from_min_size(bar.min + vec2(12.0, 6.0), vec2(24.0, 24.0)), Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0)), Color32::WHITE);
        }
        spaced(&p, bar.min + vec2(46.0, 18.0), Align2::LEFT_CENTER, "BLOCKBREACH", head(15.0), TEXT, 2.5);
        spaced(&p, bar.min + vec2(170.0, 18.0), Align2::LEFT_CENTER, &format!("v{}", core::VERSION), mono(10.0), FAINT, 1.0);
        // which game is up, like radio channels
        let mut x = bar.max.x - 300.0;
        for (name, live, color) in [("RON", self.ron_live, BLUE), ("MC", self.mc_live, GREEN)] {
            lamp(&p, pos2(x, bar.center().y), color, live);
            spaced(&p, pos2(x + 12.0, bar.center().y), Align2::LEFT_CENTER, name, mono(10.0), if live { TEXT } else { FAINT }, 1.5);
            x += 64.0;
        }
        let close = Rect::from_min_size(pos2(bar.max.x - 46.0, bar.min.y), vec2(46.0, 36.0));
        let min = Rect::from_min_size(pos2(bar.max.x - 92.0, bar.min.y), vec2(46.0, 36.0));
        for (r, is_close) in [(min, false), (close, true)] {
            let resp = ui.interact(r, ui.id().with(("winbtn", is_close)), Sense::click());
            if resp.hovered() {
                p.rect_filled(r, CornerRadius::ZERO, if is_close { Color32::from_rgb(196, 43, 28) } else { with_alpha(Color32::WHITE, 0.08) });
            }
            let c = r.center();
            let s = Stroke::new(1.5, TEXT);
            if is_close {
                p.line_segment([c + vec2(-5.0, -5.0), c + vec2(5.0, 5.0)], s);
                p.line_segment([c + vec2(5.0, -5.0), c + vec2(-5.0, 5.0)], s);
            } else {
                p.line_segment([c + vec2(-5.0, 0.0), c + vec2(5.0, 0.0)], s);
            }
            if resp.clicked() {
                ui.ctx().send_viewport_cmd(if is_close { egui::ViewportCommand::Close } else { egui::ViewportCommand::Minimized(true) });
            }
        }
    }

    fn nav(&mut self, ui: &mut egui::Ui, full: Rect) {
        let tr = self.tr();
        let p = ui.painter().clone();
        // the badge and the name
        let x = full.min.x + 28.0;
        if let Some(logo) = &self.logo {
            p.image(logo.id(), Rect::from_min_size(pos2(x, full.min.y + 62.0), vec2(56.0, 56.0)), Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0)), Color32::WHITE);
        }
        spaced(&p, pos2(x + 70.0, full.min.y + 80.0), Align2::LEFT_CENTER, "BLOCKBREACH", head(24.0), TEXT, 2.0);
        spaced(&p, pos2(x + 71.0, full.min.y + 104.0), Align2::LEFT_CENTER, "RON x MINECRAFT", mono(10.0), DIM, 2.0);
        let items = [(Page::Play, tr.nav_play), (Page::Install, tr.nav_install), (Page::Settings, tr.nav_settings), (Page::Controls, tr.nav_controls), (Page::About, tr.nav_about)];
        let mut y = full.min.y + 170.0;
        for (i, (page, label)) in items.into_iter().enumerate() {
            let r = Rect::from_min_size(pos2(full.min.x, y), vec2(SIDE - 1.0, 46.0));
            if menu_item(ui, r, i + 1, label, self.page == page).clicked() {
                self.page = page;
            }
            y += 52.0;
        }
        // language, at the bottom of the column
        let ly = full.max.y - 54.0;
        spaced(&p, pos2(x, ly), Align2::LEFT_CENTER, tr.language.to_uppercase().as_str(), mono(10.0), FAINT, 1.5);
        let mut lx = x + 100.0;
        for (code, label) in [("en", "EN"), ("ru", "RU")] {
            let r = Rect::from_center_size(pos2(lx, ly), vec2(40.0, 24.0));
            let resp = ui.interact(r, ui.id().with(("lang", code)), Sense::click());
            let on = self.state.settings.language == code;
            if on {
                p.rect_stroke(r, CornerRadius::ZERO, Stroke::new(1.0, TEXT), StrokeKind::Inside);
            }
            p.text(r.center(), Align2::CENTER_CENTER, label, mono(11.0), if on || resp.hovered() { TEXT } else { DIM });
            if resp.clicked() {
                self.state.settings.language = code.into();
                self.state.save();
            }
            lx += 48.0;
        }
    }

    fn page_play(&mut self, ui: &mut egui::Ui, area: Rect, t: f32) {
        let tr = self.tr();
        let p = ui.painter().clone();
        // the operation's name
        tag(&p, area.min + vec2(0.0, 8.0), tr.operation, DIM);
        let tw = spaced(&p, area.min + vec2(-3.0, 52.0), Align2::LEFT_CENTER, "BLOCKBREACH", head(66.0), TEXT, 6.0);
        spaced(&p, area.min + vec2(0.0, 98.0), Align2::LEFT_CENTER, "READY OR NOT  x  MINECRAFT", mono(13.0), DIM, 3.0);
        // the drop of Minecraft: its yellow splash, small, tilted, pulsing
        let splash = tr.splashes[self.splash % tr.splashes.len()];
        let size = (11.0 + (t * 5.0).sin().abs() * 1.0).round();
        let g = p.layout_no_wrap(splash.to_owned(), pixel(size), SPLASH);
        let gs = p.layout_no_wrap(splash.to_owned(), pixel(size), Color32::from_rgb(63, 63, 21));
        let anchor = pos2(area.min.x + (tw - g.size().x * 0.8).min(area.width() - g.size().x - 10.0), area.min.y + 18.0);
        p.add(egui::epaint::TextShape::new(anchor + vec2(1.5, 1.5), gs, Color32::from_rgb(63, 63, 21)).with_angle(-0.12));
        p.add(egui::epaint::TextShape::new(anchor, g, SPLASH).with_angle(-0.12));

        // objectives
        let installed = self.installed_ok();
        let obj = Rect::from_min_size(area.min + vec2(0.0, 126.0), vec2(area.width(), 196.0));
        panel(&p, obj, Some(tr.h_objectives));
        let state = |ok: bool, pending: bool| if ok { tr.st_done } else if pending { tr.st_pending } else { tr.st_failed };
        let rows = [
            (
                Some(self.state.ron_dir.is_some()),
                tr.obj_ron,
                self.state.ron_dir.as_ref().map(|d| pretty(d)).unwrap_or_else(|| tr.not_found.into()),
                state(self.state.ron_dir.is_some(), false),
            ),
            (
                if self.launcher.is_some() { Some(true) } else { Some(false) },
                tr.obj_mc,
                if self.launcher.is_some() { pretty(&self.state.mc_dir.clone().unwrap_or_default()) } else { tr.launcher_missing.into() },
                state(self.launcher.is_some(), false),
            ),
            (
                match installed {
                    Some(true) => Some(true),
                    Some(false) => Some(false),
                    None => None,
                },
                tr.obj_mod,
                match installed {
                    Some(true) => format!("{} - v{}", tr.mod_installed, self.state.installed.as_ref().map(|i| i.version.clone()).unwrap_or_default()),
                    Some(false) => tr.mod_broken.into(),
                    None => tr.mod_not_installed.into(),
                },
                state(installed == Some(true), installed.is_none()),
            ),
        ];
        let mut y = obj.min.y + 42.0;
        for (done, text, detail, st) in rows {
            objective(&p, pos2(obj.min.x + 20.0, y), obj.width() - 40.0, done, text, &ellipsize(&detail, 92), st);
            y += 50.0;
        }

        // READY (or DEPLOY when there is nothing installed yet)
        let btn = Rect::from_min_size(pos2(area.min.x, obj.max.y + 26.0), vec2(300.0, 64.0));
        let playing = self.play_thread.as_ref().map(|h| !h.is_finished()).unwrap_or(false);
        if installed.is_some() {
            if button(ui, btn, tr.play, head(30.0), Btn::Primary, installed == Some(true) && !playing && self.launcher.is_some()).clicked() {
                self.start_play();
            }
        } else if button(ui, btn, tr.install, head(28.0), Btn::Primary, self.state.ron_dir.is_some() && !self.busy()).clicked() {
            self.page = Page::Install;
            self.start_job(JobKind::Install);
        }
        if installed == Some(false) {
            let r = Rect::from_min_size(pos2(btn.max.x + 16.0, btn.min.y + 12.0), vec2(180.0, 40.0));
            if button(ui, r, tr.repair, head_m(17.0), Btn::Ghost, !self.busy()).clicked() {
                self.page = Page::Install;
                self.start_job(JobKind::Install);
            }
        } else {
            p.text(pos2(btn.max.x + 20.0, btn.center().y), Align2::LEFT_CENTER, tr.play_sub, body(15.0), DIM);
        }

        // what PLAY is doing
        let y0 = btn.max.y + 28.0;
        let x0 = area.min.x;
        match &self.play.get() {
            PlayStep::Idle => {
                if installed.is_none() {
                    p.text(pos2(x0, y0), Align2::LEFT_CENTER, tr.install_first, body(16.0), DIM);
                }
            }
            PlayStep::Done => {
                p.rect_filled(Rect::from_center_size(pos2(x0 + 6.0, y0), vec2(8.0, 8.0)), CornerRadius::ZERO, GREEN);
                p.text(pos2(x0 + 22.0, y0), Align2::LEFT_CENTER, tr.step_done, body_b(16.0), GREEN);
            }
            PlayStep::Failed(e) => {
                p.rect_filled(Rect::from_center_size(pos2(x0 + 6.0, y0), vec2(8.0, 8.0)), CornerRadius::ZERO, RED);
                p.text(pos2(x0 + 22.0, y0), Align2::LEFT_CENTER, format!("{}: {e}", tr.step_failed), body_b(16.0), RED);
            }
            s => {
                spinner(&p, pos2(x0 + 8.0, y0), t, TEXT);
                let text = match s {
                    PlayStep::StartingLauncher => tr.step_launcher,
                    PlayStep::WaitingForMinecraft => tr.step_wait_mc,
                    PlayStep::WaitingForWorld => tr.step_wait_world,
                    PlayStep::StartingRon => tr.step_ron,
                    _ => tr.step_wait_ron,
                };
                p.text(pos2(x0 + 26.0, y0), Align2::LEFT_CENTER, text, body_b(16.0), TEXT);
                p.text(pos2(x0 + 26.0, y0 + 22.0), Align2::LEFT_CENTER, tr.cancel_hint, body(14.0), DIM);
            }
        }

        self.loadout(&p, area, t);
    }

    /// The bottom row of the Play page: what the mod does, each with a Minecraft block.
    fn loadout(&self, p: &egui::Painter, area: Rect, t: f32) {
        let tr = self.tr();
        let top = area.max.y - 96.0;
        tag(p, pos2(area.min.x, top - 16.0), tr.h_loadout, DIM);
        let n = tr.features.len() as f32;
        let gap = 12.0;
        let w = (area.width() - gap * (n - 1.0)) / n;
        for (i, (head_text, text)) in tr.features.iter().enumerate() {
            let r = Rect::from_min_size(pos2(area.min.x + i as f32 * (w + gap), top), vec2(w, 90.0));
            p.rect_filled(r, CornerRadius::ZERO, PANEL);
            p.rect_stroke(r, CornerRadius::ZERO, Stroke::new(1.0, LINE), StrokeKind::Inside);
            p.rect_filled(Rect::from_min_size(r.min, vec2(w, 2.0)), CornerRadius::ZERO, with_alpha(Color32::WHITE, 0.35));
            let block = [Block::Brick, Block::Grass, Block::Tnt][i % 3];
            let bob = (t * 1.6 + i as f32 * 1.3).sin() * 2.0;
            cube(p, pos2(r.min.x + 40.0, r.center().y + bob), 40.0, block, 1.0, 40 + i as u32);
            spaced(p, pos2(r.min.x + 76.0, r.min.y + 26.0), Align2::LEFT_CENTER, head_text, head(18.0), TEXT, 2.0);
            let g = p.layout(text.to_string(), body(14.5), DIM, r.max.x - r.min.x - 88.0);
            p.galley(pos2(r.min.x + 76.0, r.min.y + 42.0), g, DIM);
        }
    }

    fn page_title(p: &egui::Painter, area: Rect, title: &str) {
        spaced(p, area.min + vec2(-2.0, 22.0), Align2::LEFT_CENTER, title, head(40.0), TEXT, 4.0);
    }

    fn page_install(&mut self, ui: &mut egui::Ui, area: Rect, t: f32) {
        let tr = self.tr();
        let p = ui.painter().clone();
        Self::page_title(&p, area, tr.nav_install);

        let what = Rect::from_min_size(area.min + vec2(0.0, 54.0), vec2(area.width(), 140.0));
        panel(&p, what, Some(tr.h_manifest));
        for (i, l) in tr.what_install.iter().enumerate() {
            let y = what.min.y + 46.0 + i as f32 * 24.0;
            p.text(pos2(what.min.x + 20.0, y), Align2::LEFT_CENTER, format!("{:02}", i + 1), mono(11.0), FAINT);
            p.text(pos2(what.min.x + 48.0, y), Align2::LEFT_CENTER, *l, body(15.5), TEXT);
        }

        // folders
        let busy = self.busy();
        let mut y = what.max.y + 20.0;
        for (label, which) in [(tr.ron_folder, 0), (tr.mc_folder, 1)] {
            spaced(&p, pos2(area.min.x, y + 6.0), Align2::LEFT_CENTER, &label.to_uppercase(), mono(10.0), DIM, 1.5);
            let field = Rect::from_min_size(pos2(area.min.x, y + 18.0), vec2(area.width() - 132.0, 32.0));
            let text = if which == 0 { &mut self.ron_text } else { &mut self.mc_text };
            ui.put(field, egui::TextEdit::singleline(text).font(body(16.0)).margin(vec2(10.0, 7.0)).interactive(!busy));
            let b = Rect::from_min_size(pos2(field.max.x + 12.0, field.min.y), vec2(120.0, 32.0));
            if button(ui, b, &tr.browse.to_uppercase(), head_m(14.0), Btn::Ghost, !busy).clicked() {
                if let Some(dir) = rfd::FileDialog::new().set_directory(if which == 0 { &self.ron_text } else { &self.mc_text }).pick_folder() {
                    let s = dir.display().to_string();
                    if which == 0 {
                        // accept the game folder or anything inside it
                        let mut d = Some(dir.as_path());
                        let mut found = None;
                        while let Some(x) = d {
                            if core::ron_valid(x) {
                                found = Some(x.to_path_buf());
                                break;
                            }
                            d = x.parent();
                        }
                        self.ron_text = found.map(|f| f.display().to_string()).unwrap_or(s);
                        self.path_error = (!core::ron_valid(std::path::Path::new(&self.ron_text))).then(|| tr.bad_ron_folder.to_string());
                    } else {
                        self.mc_text = s;
                    }
                }
            }
            y += 60.0;
        }
        if let Some(e) = &self.path_error {
            p.text(pos2(area.min.x, y + 2.0), Align2::LEFT_CENTER, e, body(14.5), RED);
            y += 18.0;
        }

        // options
        y += 4.0;
        let mut changed = false;
        changed |= toggle(ui, pos2(area.min.x, y), area.width(), &mut self.state.settings.dedicated_gpu, tr.opt_gpu, None);
        changed |= toggle(ui, pos2(area.min.x, y + 28.0), 330.0, &mut self.state.settings.desktop_shortcut, tr.opt_desktop, None);
        changed |= toggle(ui, pos2(area.min.x + 340.0, y + 28.0), area.width() - 340.0, &mut self.state.settings.keep_world, tr.opt_keep_world, None);
        if changed {
            self.state.save();
        }
        y += 72.0;

        // buttons: the demo shows DEPLOY until its slowed-down bar is full
        let demo_filling = self.demo.is_some() && self.job_started.map(|s| s.elapsed().as_secs_f32() < 6.0).unwrap_or(false);
        let installed = self.state.installed.is_some() && !demo_filling;
        let main = Rect::from_min_size(pos2(area.min.x, y), vec2(260.0, 52.0));
        if button(ui, main, if installed { tr.repair } else { tr.install }, head(22.0), Btn::Primary, !busy).clicked() {
            self.start_job(JobKind::Install);
        }
        if installed {
            let r = Rect::from_min_size(pos2(main.max.x + 14.0, y), vec2(230.0, 52.0));
            let label = if self.confirm_uninstall { tr.sure } else { tr.uninstall };
            if button(ui, r, label, head_m(17.0), Btn::Danger, !busy).clicked() {
                if self.confirm_uninstall {
                    self.confirm_uninstall = false;
                    self.start_job(JobKind::Uninstall);
                } else {
                    self.confirm_uninstall = true;
                }
            }
        }
        y += 70.0;

        // progress (Minecraft's own loading bar) and the log
        let pr = self.progress.0.lock().unwrap();
        if pr.running || pr.done.is_some() {
            let failed = matches!(pr.done, Some(Err(_)));
            let bar = Rect::from_min_size(pos2(area.min.x, y), vec2(area.width() - 70.0, 20.0));
            let shown_fraction = match (self.demo, self.job_started) {
                (Some(_), Some(s)) => pr.fraction.min(s.elapsed().as_secs_f32() / 6.0),
                _ => pr.fraction,
            };
            progress_bar(&p, bar, shown_fraction, failed);
            spaced(&p, pos2(area.max.x, bar.center().y), Align2::RIGHT_CENTER, &format!("{}%", (shown_fraction * 100.0) as u32), mono(13.0), TEXT, 1.0);
            let mut ly = bar.max.y + 16.0;
            // in the demo the log lines come in step with the bar
            let total = if self.demo.is_some() && pr.fraction > 0.0 {
                ((shown_fraction / pr.fraction.max(0.01)) * pr.lines.len() as f32).ceil() as usize
            } else {
                pr.lines.len()
            };
            let lines = &pr.lines[..total.min(pr.lines.len())];
            let shown: Vec<_> = lines.iter().rev().take(((area.max.y - ly) / 19.0).max(1.0) as usize).cloned().collect();
            for (kind, text) in shown.into_iter().rev() {
                let c = match kind {
                    LineKind::Ok => GREEN,
                    LineKind::Warn => AMBER,
                    LineKind::Error => RED,
                    LineKind::Info => DIM,
                };
                p.text(pos2(area.min.x, ly), Align2::LEFT_CENTER, ellipsize(&format!("> {text}"), 100), mono(11.5), c);
                ly += 19.0;
            }
            if pr.running {
                spinner(&p, pos2(area.max.x - 70.0 - 14.0, bar.center().y), t, Color32::WHITE);
            }
            let needs_admin = pr.needs_admin && failed;
            drop(pr);
            if needs_admin {
                let r = Rect::from_min_size(pos2(area.max.x - 240.0, y - 70.0), vec2(240.0, 52.0));
                if button(ui, r, tr.as_admin, head_m(15.0), Btn::Ghost, true).clicked() && core::restart_as_admin("--page install") {
                    ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
                }
            }
        }
    }

    fn page_settings(&mut self, ui: &mut egui::Ui, area: Rect) {
        let tr = self.tr();
        let p = ui.painter().clone();
        Self::page_title(&p, area, tr.settings_title);
        let box_ = Rect::from_min_size(area.min + vec2(0.0, 54.0), vec2(area.width(), 300.0));
        panel(&p, box_, Some(tr.h_game));
        let x = box_.min.x + 22.0;
        let mut y = box_.min.y + 42.0;
        let mut changed = slider(ui, Rect::from_min_size(pos2(x, y), vec2(box_.width() - 44.0, 44.0)), &mut self.state.settings.mc_scale, 25, 100, 5, tr.mc_scale);
        p.text(pos2(x, y + 60.0), Align2::LEFT_CENTER, tr.mc_scale_hint, body(14.5), DIM);
        y += 90.0;
        changed |= toggle(ui, pos2(x, y), box_.width() - 44.0, &mut self.state.settings.mobs_hunt_player, tr.hunt, Some(tr.hunt_hint));
        y += 60.0;
        changed |= toggle(ui, pos2(x, y), box_.width() - 44.0, &mut self.state.settings.squad_fights_mobs, tr.squad, Some(tr.squad_hint));
        if changed {
            self.saved_at = None;
        }
        let installed = self.state.installed.clone();
        let b = Rect::from_min_size(pos2(area.min.x, box_.max.y + 22.0), vec2(220.0, 50.0));
        if button(ui, b, tr.save, head(20.0), Btn::Primary, installed.is_some()).clicked() {
            self.state.save();
            if let Some(inst) = &installed {
                if core::write_config(&inst.ron_win64, &self.state.settings).is_ok() {
                    self.saved_at = Some(Instant::now());
                }
            }
        }
        let note = if installed.is_none() {
            Some((tr.settings_next_start, DIM))
        } else {
            self.saved_at.filter(|s| s.elapsed().as_secs_f32() < 6.0).map(|_| (tr.saved, GREEN))
        };
        if let Some((n, c)) = note {
            let g = p.layout(n.to_string(), body_b(15.0), c, area.max.x - b.max.x - 20.0);
            let h = g.size().y;
            p.galley(pos2(b.max.x + 18.0, b.center().y - h / 2.0), g, c);
        }
    }

    fn page_controls(&mut self, ui: &mut egui::Ui, area: Rect) {
        let tr = self.tr();
        let p = ui.painter().clone();
        Self::page_title(&p, area, tr.controls_title);
        let box_ = Rect::from_min_size(area.min + vec2(0.0, 54.0), vec2(area.width(), 38.0 * tr.controls.len() as f32 + 44.0));
        panel(&p, box_, Some(tr.h_keys));
        let mut y = box_.min.y + 40.0;
        for (key, what) in tr.controls {
            keycap(&p, pos2(box_.min.x + 20.0, y), key);
            p.text(pos2(box_.min.x + 130.0, y + 14.0), Align2::LEFT_CENTER, *what, body(16.0), TEXT);
            y += 38.0;
        }
        // the hotbar, drawn as Minecraft draws it: the one fully-Minecraft thing here
        let hb_y = box_.max.y + 30.0;
        tag(&p, pos2(area.min.x, hb_y), tr.hotbar_title, DIM);
        let slot = 54.0;
        let hb = Rect::from_min_size(pos2(area.min.x, hb_y + 16.0), vec2(slot * 9.0 + 6.0, slot + 6.0));
        p.rect_filled(hb, CornerRadius::ZERO, Color32::BLACK);
        for i in 0..9 {
            let r = Rect::from_min_size(hb.min + vec2(3.0 + i as f32 * slot, 3.0), vec2(slot, slot)).shrink(2.0);
            p.rect_filled(r, CornerRadius::ZERO, Color32::from_rgb(139, 139, 139));
            p.rect_stroke(r, CornerRadius::ZERO, Stroke::new(2.0, Color32::from_rgb(55, 55, 55)), StrokeKind::Inside);
            item_icon(&p, r.center(), i);
            p.text(r.left_top() + vec2(5.0, 4.0), Align2::LEFT_TOP, (i + 1).to_string(), pixel(8.0), Color32::WHITE);
            let resp = ui.interact(r, ui.id().with(("slot", i)), Sense::hover());
            if resp.hovered() {
                p.rect_stroke(r, CornerRadius::ZERO, Stroke::new(2.0, Color32::WHITE), StrokeKind::Inside);
            }
        }
        // the slot names under it, so nothing needs hovering: three columns
        for (i, name) in tr.hotbar.iter().enumerate() {
            let col = (i / 3) as f32;
            let row = (i % 3) as f32;
            let at = pos2(hb.min.x + col * 200.0, hb.max.y + 18.0 + row * 20.0);
            p.text(at, Align2::LEFT_CENTER, format!("{}", i + 1), mono(11.0), FAINT);
            p.text(at + vec2(18.0, 0.0), Align2::LEFT_CENTER, *name, body(14.5), DIM);
        }
    }

    fn page_about(&mut self, ui: &mut egui::Ui, area: Rect) {
        let tr = self.tr();
        let p = ui.painter().clone();
        Self::page_title(&p, area, tr.about_title);
        let text = tr.about.join(" ");
        let g = p.layout(text, body(16.0), TEXT, area.width() - 40.0);
        let a = Rect::from_min_size(area.min + vec2(0.0, 54.0), vec2(area.width(), g.size().y + 56.0));
        panel(&p, a, Some("BLOCKBREACH"));
        p.galley(a.min + vec2(20.0, 38.0), g, TEXT);
        let c = Rect::from_min_size(pos2(area.min.x, a.max.y + 18.0), vec2(area.width(), 44.0 + 24.0 * tr.credits.len() as f32));
        panel(&p, c, Some(&tr.credits_title.to_uppercase()));
        for (i, l) in tr.credits.iter().enumerate() {
            p.text(pos2(c.min.x + 20.0, c.min.y + 48.0 + i as f32 * 24.0), Align2::LEFT_CENTER, *l, body(15.5), if i + 1 == tr.credits.len() { DIM } else { TEXT });
        }
        let b = Rect::from_min_size(pos2(area.min.x, c.max.y + 22.0), vec2(260.0, 46.0));
        if button(ui, b, &tr.open_folder.to_uppercase(), head_m(16.0), Btn::Ghost, true).clicked() {
            let dir = core::app_dir();
            let _ = std::fs::create_dir_all(&dir);
            let lic = dir.join("licenses");
            let _ = std::fs::create_dir_all(&lic);
            for (name, text) in core::license_texts() {
                let _ = std::fs::write(lic.join(name), text);
            }
            core::open_folder(&dir);
        }
    }
}

/// Little pixel icons for the hotbar slots.
fn item_icon(p: &egui::Painter, c: Pos2, slot: usize) {
    let px = |p: &egui::Painter, x: f32, y: f32, col: u32| {
        p.rect_filled(Rect::from_min_size(c + vec2(x * 3.0, y * 3.0), vec2(3.0, 3.0)), CornerRadius::ZERO, Color32::from_rgb((col >> 16) as u8, (col >> 8) as u8, col as u8));
    };
    match slot {
        0 | 6 => {
            // spawn egg: an oval with spots (zombie: teal/green, creeper: green/black)
            let (base, spot) = if slot == 0 { (0x00afaf, 0x799c65) } else { (0x0da70b, 0x000000) };
            for y in -5..5 {
                let w = match y { -5 | 4 => 2, -4 | 3 => 3, _ => 4 };
                for x in -w..w {
                    let s = ((x * 7 + y * 13) as i32).rem_euclid(5) == 0;
                    px(p, x as f32, y as f32, if s { spot } else { base });
                }
            }
        }
        1 => {
            for i in 0..9 {
                px(p, -4.0 + i as f32, 4.0 - i as f32, if i < 2 { 0x6b4e2a } else { 0x5decf5 });
            }
            px(p, -3.0, 2.0, 0x3a3a3a);
            px(p, -2.0, 3.0, 0x3a3a3a);
        }
        2 | 3 => {
            // bow / crossbow
            for i in -5..=5 {
                let x = if slot == 3 { (5 - (i as i32).abs()) as f32 / 2.0 - 2.0 } else { 1.0 - ((i * i) as f32) / 12.0 };
                px(p, x, i as f32, 0x7a5a32);
            }
            for i in -5..=5 {
                px(p, if slot == 3 { -2.5 } else { -2.0 }, i as f32, 0xdddddd);
            }
            if slot == 2 {
                for i in -4..=4 {
                    px(p, i as f32, 0.0, 0x5a4020);
                }
            }
        }
        4 | 7 => {
            cube(p, c, 34.0, if slot == 4 { Block::Tnt } else { Block::Grass }, 1.0, 11);
        }
        5 => {
            for i in 0..5 {
                px(p, -3.0 + i as f32, 2.0 - i as f32 * 0.5, 0x9a9a9a);
            }
            px(p, 2.0, -1.0, 0xff8a2a);
            px(p, -4.0, 3.0, 0x444444);
            px(p, -3.0, 3.0, 0x444444);
        }
        _ => {
            for y in -4..4 {
                px(p, -1.0, y as f32, 0xd03030);
                px(p, 0.0, y as f32, 0xe04848);
            }
            px(p, -1.0, -5.0, 0xdddddd);
            px(p, 0.0, -5.0, 0xdddddd);
            for y in 4..6 {
                px(p, -0.5, y as f32, 0x8a6a3a);
            }
        }
    }
}

/// A path as Windows shows it (proper case, no \?\ prefix).
fn pretty(p: &std::path::Path) -> String {
    let s = std::fs::canonicalize(p).map(|c| c.display().to_string()).unwrap_or_else(|_| p.display().to_string());
    s.strip_prefix(r"\\?\").map(str::to_owned).unwrap_or(s)
}

fn ellipsize(s: &str, max: usize) -> String {
    if s.chars().count() <= max {
        s.to_owned()
    } else {
        let keep: String = s.chars().take(max - 3).collect();
        keep + "..."
    }
}

fn save_bmp(path: &std::path::Path, img: &egui::ColorImage) {
    let (w, h) = (img.size[0] as u32, img.size[1] as u32);
    let mut out = Vec::with_capacity(54 + (w * h * 4) as usize);
    let size = 54 + w * h * 4;
    out.extend_from_slice(b"BM");
    for v in [size, 0, 54, 40] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out.extend_from_slice(&(w as i32).to_le_bytes());
    out.extend_from_slice(&(h as i32).to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&32u16.to_le_bytes());
    for v in [0u32, w * h * 4, 2835, 2835, 0, 0] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    for y in (0..h).rev() {
        for x in 0..w {
            let c = img.pixels[(y * w + x) as usize];
            out.extend_from_slice(&[c.b(), c.g(), c.r(), 255]);
        }
    }
    let _ = std::fs::write(path, out);
}

/// `--selftest <dir>`: install into a fake game layout under <dir>, check, repair, uninstall, check; the report goes to
/// <dir>\selftest.txt.
fn selftest(dir: PathBuf) {
    use std::fmt::Write as _;
    let mut out = String::new();
    let mut ok = true;
    let mut check = |what: &str, cond: bool, out: &mut String| {
        ok &= cond;
        let _ = writeln!(out, "{} {what}", if cond { "PASS" } else { "FAIL" });
    };
    let ron = dir.join("Ready Or Not");
    let w64 = core::win64(&ron);
    let mc = dir.join(".minecraft");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&w64).unwrap();
    std::fs::create_dir_all(&mc).unwrap();
    std::fs::write(w64.join(core::RON_EXE), b"exe").unwrap();
    std::fs::write(w64.join("dxgi.dll"), b"ORIGINAL dxgi").unwrap();
    std::fs::write(mc.join("launcher_profiles.json"), r#"{"profiles":{"x":{"name":"mine","type":"custom"}},"settings":{},"version":3}"#).unwrap();
    std::env::set_var("BLOCKBREACH_SELFTEST", "1");
    std::env::set_var("BLOCKBREACH_HOME", dir.join("home"));
    let mut state = State::default();
    state.ron_dir = Some(ron.clone());
    state.mc_dir = Some(mc.clone());
    state.settings.mc_scale = 60;
    state.settings.desktop_shortcut = false;
    let progress = Progress::default();
    progress.start();
    let state = core::install(core::Job { state, progress: progress.clone() });
    let r = progress.0.lock().unwrap().done.clone();
    check(&format!("install finished: {r:?}"), matches!(r, Some(Ok(_))), &mut out);
    check("UE4SS proxy dll", w64.join("dwmapi.dll").is_file(), &mut out);
    check("host mod", w64.join("ue4ss/Mods/RoNPassthrough/dlls/main.dll").is_file(), &mut out);
    let cfg = std::fs::read_to_string(w64.join("ue4ss/Mods/RoNPassthrough/config.ini")).unwrap_or_default();
    check("config.ini has mc_scale=60", cfg.contains("mc_scale=60"), &mut out);
    check("dxgi.dll replaced", std::fs::read(w64.join("dxgi.dll")).map(|d| d.len() > 1000).unwrap_or(false), &mut out);
    check("dxgi.dll backed up", std::fs::read(dir.join("home/backup/Win64/dxgi.dll")).map(|d| d == b"ORIGINAL dxgi").unwrap_or(false), &mut out);
    let prof = std::fs::read_to_string(mc.join("launcher_profiles.json")).unwrap_or_default();
    check("profile added, the user's profile kept", prof.contains(r#""blockbreach""#) && prof.contains(r#""mine""#), &mut out);
    check("fabric version json", mc.join(format!("versions/{0}/{0}.json", core::FABRIC_VERSION)).is_file(), &mut out);
    check("mods in the game dir", dir.join("home/minecraft/mods/blockbreach-passthrough.jar").is_file(), &mut out);
    check("install intact", state.installed.as_ref().map(core::install_intact).unwrap_or(false), &mut out);
    // repair over an install: the backup stays the original
    let progress = Progress::default();
    progress.start();
    let state = core::install(core::Job { state, progress: progress.clone() });
    check("repair finished", matches!(progress.0.lock().unwrap().done, Some(Ok(_))), &mut out);
    check("backup still the original after repair", std::fs::read(dir.join("home/backup/Win64/dxgi.dll")).map(|d| d == b"ORIGINAL dxgi").unwrap_or(false), &mut out);
    std::fs::create_dir_all(dir.join("home/minecraft/saves/passthrough")).unwrap();
    std::fs::write(dir.join("home/minecraft/saves/passthrough/level.dat"), b"world").unwrap();
    std::fs::write(w64.join("ue4ss/Mods/RoNPassthrough/history.log"), b"log").unwrap();
    let progress = Progress::default();
    progress.start();
    let state = core::uninstall(core::Job { state, progress: progress.clone() });
    let r = progress.0.lock().unwrap().done.clone();
    check(&format!("uninstall finished: {r:?}"), matches!(r, Some(Ok(_))), &mut out);
    check("dxgi.dll restored", std::fs::read(w64.join("dxgi.dll")).map(|d| d == b"ORIGINAL dxgi").unwrap_or(false), &mut out);
    check("ue4ss folder gone (with the mod's log)", !w64.join("ue4ss").exists(), &mut out);
    check("dwmapi.dll gone", !w64.join("dwmapi.dll").exists(), &mut out);
    check("reshade-shaders gone", !w64.join("reshade-shaders").exists(), &mut out);
    check("ReShade.ini gone", !w64.join("ReShade.ini").exists(), &mut out);
    check("game exe untouched", std::fs::read(w64.join(core::RON_EXE)).map(|d| d == b"exe").unwrap_or(false), &mut out);
    let prof = std::fs::read_to_string(mc.join("launcher_profiles.json")).unwrap_or_default();
    check("profile removed, the user's kept", !prof.contains(r#""blockbreach""#) && prof.contains(r#""mine""#), &mut out);
    check("fabric version removed", !mc.join("versions").join(core::FABRIC_VERSION).exists(), &mut out);
    check("world kept", dir.join("home/minecraft/saves/passthrough/level.dat").is_file(), &mut out);
    check("mods removed", !dir.join("home/minecraft/mods").exists(), &mut out);
    check("state says not installed", state.installed.is_none(), &mut out);
    let _ = writeln!(out, "{}", if ok { "ALL PASSED" } else { "SOME FAILED" });
    let _ = std::fs::write(dir.join("selftest.txt"), out);
}

fn main() -> eframe::Result {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a == "--install-now") {
        // no UI: install or repair with the saved (or found) folders and settings (for scripts)
        let mut state = State::load();
        if state.ron_dir.as_ref().map(|d| !core::ron_valid(d)).unwrap_or(true) {
            state.ron_dir = core::find_ron();
        }
        if state.mc_dir.is_none() {
            state.mc_dir = Some(core::default_mc_dir());
        }
        let progress = Progress::default();
        progress.start();
        core::install(core::Job { state, progress: progress.clone() });
        let pr = progress.0.lock().unwrap();
        let log: Vec<String> = pr.lines.iter().map(|(_, l)| l.clone()).collect();
        let _ = std::fs::write(core::app_dir().join("install.log"), log.join("
"));
        return Ok(());
    }
    if args.iter().any(|a| a == "--uninstall-now") {
        // no UI: uninstall what state.json records (for scripts)
        let progress = Progress::default();
        progress.start();
        core::uninstall(core::Job { state: State::load(), progress: progress.clone() });
        let pr = progress.0.lock().unwrap();
        let log: Vec<String> = pr.lines.iter().map(|(_, l)| l.clone()).collect();
        let _ = std::fs::write(core::app_dir().join("uninstall.log"), log.join("\n"));
        return Ok(());
    }
    if let Some(w) = args.windows(2).find(|w| w[0] == "--selftest") {
        selftest(PathBuf::from(&w[1]));
        return Ok(());
    }
    let shot = args.windows(3).find(|w| w[0] == "--shot").map(|w| (w[1].clone(), PathBuf::from(&w[2])));
    let uninstall = args.iter().any(|a| a == "--uninstall");
    let mut page = if uninstall || args.windows(2).any(|w| w[0] == "--page" && w[1] == "install") { Page::Install } else { Page::Play };
    if let Some((name, _)) = &shot {
        page = match name.as_str() {
            "install" => Page::Install,
            "settings" => Page::Settings,
            "controls" => Page::Controls,
            "about" => Page::About,
            _ => Page::Play,
        };
    }
    let lang = args.windows(2).find(|w| w[0] == "--lang").map(|w| w[1].clone());
    let demo = args.iter().any(|a| a == "--demo");
    let rgba = include_bytes!("../assets/icon_64.rgba").to_vec();
    let mut viewport = egui::ViewportBuilder::default();
    if shot.is_some() {
        viewport = viewport.with_position([-6000.0, 100.0]).with_active(false).with_taskbar(false);
    }
    let options = eframe::NativeOptions {
        viewport: viewport
            .with_title("BlockBreach")
            .with_inner_size([W, H])
            .with_resizable(false)
            .with_decorations(false)
            .with_icon(Arc::new(egui::IconData { rgba, width: 64, height: 64 })),
        centered: shot.is_none(),
        ..Default::default()
    };
    eframe::run_native(
        "BlockBreach",
        options,
        Box::new(move |cc| {
            let mut app = App::new(cc, page, uninstall);
            if let Some((_, path)) = shot {
                app.shot = Some((path, 0));
            }
            if let Some(l) = lang {
                app.state.settings.language = l;
            }
            if demo {
                // a few seconds to start the recorder first
                app.demo = Some(Instant::now() + std::time::Duration::from_secs(4));
                app.page = Page::Play;
            }
            Ok(Box::new(app))
        }),
    )
}
