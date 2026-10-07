//! BlockBreach's look: Ready or Not's menus (a dark, slowly drifting scene, condensed uppercase type, thin lines,
//! white-on-black buttons, mission objectives) with a drop of Minecraft (pixel blocks, the green loading bar, a
//! yellow splash, the hotbar).

use eframe::egui::{self, pos2, vec2, Align2, Color32, CornerRadius, FontFamily, FontId, Painter, Pos2, Rect, Response, Sense, Shape, Stroke, StrokeKind, Ui};

thread_local! {
    /// Demo mode's fake pointer (window coordinates) and whether it is pressed: buttons react to it as to the mouse.
    pub static VIRTUAL: std::cell::Cell<Option<(Pos2, bool)>> = const { std::cell::Cell::new(None) };
}

fn virtual_on(rect: Rect) -> (bool, bool) {
    VIRTUAL.with(|v| match v.get() {
        Some((p, down)) if rect.contains(p) => (true, down),
        _ => (false, false),
    })
}

pub const BG: Color32 = Color32::from_rgb(7, 8, 10);
pub const PANEL: Color32 = Color32::from_rgba_premultiplied(9, 10, 12, 205);
pub const LINE: Color32 = Color32::from_rgba_premultiplied(30, 32, 36, 60);
pub const TEXT: Color32 = Color32::from_rgb(232, 234, 237);
pub const DIM: Color32 = Color32::from_rgb(134, 139, 148);
pub const FAINT: Color32 = Color32::from_rgb(78, 83, 92);
pub const BLUE: Color32 = Color32::from_rgb(70, 128, 222);
pub const POLICE_RED: Color32 = Color32::from_rgb(214, 56, 48);
pub const GREEN: Color32 = Color32::from_rgb(104, 196, 70); // Minecraft's grass, for "done"
pub const RED: Color32 = Color32::from_rgb(226, 82, 70);
pub const AMBER: Color32 = Color32::from_rgb(230, 178, 64);
pub const SPLASH: Color32 = Color32::from_rgb(255, 255, 85);

pub fn head(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("head".into()))
}
pub fn head_m(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("headm".into()))
}
pub fn body(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("body".into()))
}
pub fn body_b(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("bodyb".into()))
}
pub fn mono(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("mono".into()))
}
pub fn pixel(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("pixel".into()))
}

pub fn with_alpha(c: Color32, a: f32) -> Color32 {
    Color32::from_rgba_unmultiplied(c.r(), c.g(), c.b(), (a.clamp(0.0, 1.0) * 255.0) as u8)
}

/// Text with letter spacing (Ready or Not's titles are tracked out). Returns its width.
pub fn spaced(p: &Painter, pos: Pos2, align: Align2, text: &str, font: FontId, color: Color32, spacing: f32) -> f32 {
    let glyphs: Vec<_> = text.chars().map(|c| p.layout_no_wrap(c.to_string(), font.clone(), color)).collect();
    let width: f32 = glyphs.iter().map(|g| g.size().x).sum::<f32>() + spacing * (glyphs.len().saturating_sub(1)) as f32;
    let height = glyphs.iter().map(|g| g.size().y).fold(0.0, f32::max);
    let mut x = match align.x() {
        egui::Align::Min => pos.x,
        egui::Align::Center => pos.x - width / 2.0,
        egui::Align::Max => pos.x - width,
    };
    let y = match align.y() {
        egui::Align::Min => pos.y,
        egui::Align::Center => pos.y - height / 2.0,
        egui::Align::Max => pos.y - height,
    };
    for g in glyphs {
        let w = g.size().x;
        p.galley(pos2(x, y), g, color);
        x += w + spacing;
    }
    width
}

/// A small "// LABEL" section tag in mono, with a short rule after it.
pub fn tag(p: &Painter, pos: Pos2, text: &str, color: Color32) -> f32 {
    let w = spaced(p, pos, Align2::LEFT_CENTER, text, mono(11.0), color, 1.5);
    p.line_segment([pos2(pos.x + w + 10.0, pos.y), pos2(pos.x + w + 46.0, pos.y)], Stroke::new(1.0, with_alpha(color, 0.5)));
    w
}

#[derive(Clone, Copy, PartialEq)]
pub enum Btn {
    /// white, black text: Ready or Not's READY
    Primary,
    /// a thin outline
    Ghost,
    Danger,
}

/// Ready or Not's button: flat, square, uppercase. Hover fills it; pressed sinks it a pixel.
pub fn button(ui: &mut Ui, rect: Rect, label: &str, font: FontId, kind: Btn, enabled: bool) -> Response {
    let id = ui.id().with(("btn", label, rect.min.x as i32, rect.min.y as i32));
    let resp = ui.interact(rect, id, if enabled { Sense::click() } else { Sense::hover() });
    let p = ui.painter();
    let (vh, vd) = virtual_on(rect);
    let hovered = enabled && (resp.hovered() || vh);
    let pressed = enabled && (resp.is_pointer_button_down_on() || vd);
    let r = if pressed { rect.translate(vec2(0.0, 1.0)) } else { rect };
    let (fill, stroke, text) = match (kind, enabled, hovered) {
        (_, false, _) => (with_alpha(Color32::WHITE, 0.04), with_alpha(Color32::WHITE, 0.10), FAINT),
        (Btn::Primary, true, false) => (Color32::from_rgb(232, 234, 237), Color32::from_rgb(232, 234, 237), Color32::from_rgb(10, 11, 13)),
        (Btn::Primary, true, true) => (Color32::WHITE, Color32::WHITE, Color32::BLACK),
        (Btn::Ghost, true, false) => (with_alpha(Color32::BLACK, 0.35), with_alpha(Color32::WHITE, 0.30), TEXT),
        (Btn::Ghost, true, true) => (with_alpha(Color32::WHITE, 0.92), Color32::WHITE, Color32::BLACK),
        (Btn::Danger, true, false) => (with_alpha(Color32::BLACK, 0.35), with_alpha(RED, 0.7), RED),
        (Btn::Danger, true, true) => (RED, RED, Color32::WHITE),
    };
    p.rect_filled(r, CornerRadius::ZERO, fill);
    p.rect_stroke(r, CornerRadius::ZERO, Stroke::new(1.0, stroke), StrokeKind::Inside);
    if kind == Btn::Primary && enabled {
        // the little notch Ready or Not puts on its call-to-action
        p.rect_filled(Rect::from_min_size(r.right_top() + vec2(-14.0, 0.0), vec2(14.0, 4.0)), CornerRadius::ZERO, with_alpha(BLUE, 1.0));
    }
    spaced(p, r.center(), Align2::CENTER_CENTER, label, font, text, 2.0);
    if hovered {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }
    resp
}

/// A left-menu entry in Ready or Not's main-menu style: "01  PLAY", the active one white with a bar.
pub fn menu_item(ui: &mut Ui, rect: Rect, index: usize, label: &str, selected: bool) -> Response {
    let id = ui.id().with(("menu", index));
    let resp = ui.interact(rect, id, Sense::click());
    let p = ui.painter();
    let (vh, _) = virtual_on(rect);
    let hovered = resp.hovered() || vh;
    if selected {
        let mut mesh = egui::Mesh::default();
        let (a, b) = (with_alpha(Color32::WHITE, 0.10), with_alpha(Color32::WHITE, 0.0));
        mesh.colored_vertex(rect.left_top(), a);
        mesh.colored_vertex(rect.right_top(), b);
        mesh.colored_vertex(rect.left_bottom(), a);
        mesh.colored_vertex(rect.right_bottom(), b);
        mesh.add_triangle(0, 1, 2);
        mesh.add_triangle(1, 3, 2);
        p.add(Shape::mesh(mesh));
        p.rect_filled(Rect::from_min_size(rect.min, vec2(3.0, rect.height())), CornerRadius::ZERO, TEXT);
    }
    let shift = if hovered && !selected { 6.0 } else { 0.0 };
    let color = if selected { TEXT } else if hovered { Color32::from_rgb(200, 203, 208) } else { DIM };
    p.text(pos2(rect.min.x + 18.0 + shift, rect.center().y), Align2::LEFT_CENTER, format!("{:02}", index), mono(12.0), if selected { BLUE } else { FAINT });
    spaced(p, pos2(rect.min.x + 52.0 + shift, rect.center().y), Align2::LEFT_CENTER, label, head_m(24.0), color, 2.0);
    if hovered {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }
    resp
}

/// A panel: translucent black, one thin line, a "// TITLE" tag and two small corner ticks.
pub fn panel(p: &Painter, rect: Rect, title: Option<&str>) {
    p.rect_filled(rect, CornerRadius::ZERO, PANEL);
    p.rect_stroke(rect, CornerRadius::ZERO, Stroke::new(1.0, LINE), StrokeKind::Inside);
    let s = Stroke::new(2.0, with_alpha(Color32::WHITE, 0.55));
    let l = 8.0;
    p.line_segment([rect.left_top(), rect.left_top() + vec2(l, 0.0)], s);
    p.line_segment([rect.left_top(), rect.left_top() + vec2(0.0, l)], s);
    p.line_segment([rect.right_bottom(), rect.right_bottom() - vec2(l, 0.0)], s);
    p.line_segment([rect.right_bottom(), rect.right_bottom() - vec2(0.0, l)], s);
    if let Some(t) = title {
        tag(p, rect.min + vec2(18.0, 18.0), t, DIM);
    }
}

/// A mission objective row: a square box (checked green / crossed red / empty), the text, and its state in mono.
pub fn objective(p: &Painter, at: Pos2, width: f32, done: Option<bool>, text: &str, detail: &str, state: &str) {
    let b = Rect::from_min_size(at + vec2(0.0, 2.0), vec2(16.0, 16.0));
    p.rect_stroke(b, CornerRadius::ZERO, Stroke::new(1.5, with_alpha(Color32::WHITE, 0.7)), StrokeKind::Inside);
    match done {
        Some(true) => {
            p.rect_filled(b.shrink(4.0), CornerRadius::ZERO, GREEN);
        }
        Some(false) => {
            let s = Stroke::new(2.0, RED);
            p.line_segment([b.min + vec2(4.0, 4.0), b.max - vec2(4.0, 4.0)], s);
            p.line_segment([pos2(b.max.x - 4.0, b.min.y + 4.0), pos2(b.min.x + 4.0, b.max.y - 4.0)], s);
        }
        None => {}
    }
    p.text(at + vec2(30.0, 10.0), Align2::LEFT_CENTER, text, body_b(18.0), TEXT);
    p.text(at + vec2(30.0, 31.0), Align2::LEFT_CENTER, detail, body(14.5), DIM);
    let c = match done {
        Some(true) => GREEN,
        Some(false) => RED,
        None => AMBER,
    };
    spaced(p, at + vec2(width, 10.0), Align2::RIGHT_CENTER, state, mono(11.0), c, 1.5);
}

/// An on/off option: a square box and a label with a dim hint. Returns true when clicked.
pub fn toggle(ui: &mut Ui, at: Pos2, width: f32, on: &mut bool, label: &str, hint: Option<&str>) -> bool {
    let h = if hint.is_some() { 44.0 } else { 26.0 };
    let rect = Rect::from_min_size(at, vec2(width, h));
    let resp = ui.interact(rect, ui.id().with(("toggle", label)), Sense::click());
    let p = ui.painter();
    let b = Rect::from_min_size(at + vec2(0.0, 3.0), vec2(18.0, 18.0));
    p.rect_stroke(b, CornerRadius::ZERO, Stroke::new(1.5, if resp.hovered() { Color32::WHITE } else { with_alpha(Color32::WHITE, 0.6) }), StrokeKind::Inside);
    if *on {
        p.rect_filled(b.shrink(4.5), CornerRadius::ZERO, TEXT);
    }
    p.text(at + vec2(32.0, 12.0), Align2::LEFT_CENTER, label, body_b(17.0), if resp.hovered() { Color32::WHITE } else { TEXT });
    if let Some(hint) = hint {
        p.text(at + vec2(32.0, 32.0), Align2::LEFT_CENTER, hint, body(14.5), DIM);
    }
    if resp.hovered() {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }
    if resp.clicked() {
        *on = !*on;
        return true;
    }
    false
}

/// A thin track with a square handle; the value in mono on the right.
pub fn slider(ui: &mut Ui, rect: Rect, value: &mut u32, min: u32, max: u32, step: u32, label: &str) -> bool {
    let resp = ui.interact(rect, ui.id().with(("slider", label)), Sense::click_and_drag());
    let p = ui.painter();
    p.text(pos2(rect.min.x, rect.min.y + 8.0), Align2::LEFT_CENTER, label, body_b(17.0), TEXT);
    spaced(p, pos2(rect.max.x, rect.min.y + 8.0), Align2::RIGHT_CENTER, &format!("{value}%"), mono(14.0), Color32::WHITE, 1.0);
    let ty = rect.max.y - 10.0;
    let (x0, x1) = (rect.min.x, rect.max.x);
    p.line_segment([pos2(x0, ty), pos2(x1, ty)], Stroke::new(2.0, with_alpha(Color32::WHITE, 0.18)));
    let t = (*value - min) as f32 / (max - min) as f32;
    let hx = x0 + t * (x1 - x0);
    p.line_segment([pos2(x0, ty), pos2(hx, ty)], Stroke::new(2.0, BLUE));
    for k in 0..=((max - min) / step) {
        let x = x0 + k as f32 * step as f32 / (max - min) as f32 * (x1 - x0);
        p.line_segment([pos2(x, ty + 5.0), pos2(x, ty + 8.0)], Stroke::new(1.0, with_alpha(Color32::WHITE, 0.25)));
    }
    let knob = Rect::from_center_size(pos2(hx, ty), vec2(12.0, 18.0));
    p.rect_filled(knob, CornerRadius::ZERO, if resp.hovered() || resp.dragged() { Color32::WHITE } else { TEXT });
    let mut changed = false;
    if resp.dragged() || resp.clicked() {
        if let Some(pos) = resp.interact_pointer_pos() {
            let t = ((pos.x - x0) / (x1 - x0)).clamp(0.0, 1.0);
            let v = (((min as f32 + t * (max - min) as f32) / step as f32).round() as u32 * step).clamp(min, max);
            if v != *value {
                *value = v;
                changed = true;
            }
        }
    }
    changed
}

/// Minecraft's loading bar, the one Minecraft thing on the install page: green, pixel-segmented.
pub fn progress_bar(p: &Painter, rect: Rect, fraction: f32, failed: bool) {
    p.rect_filled(rect, CornerRadius::ZERO, Color32::BLACK);
    let inner = rect.shrink(2.0);
    p.rect_filled(inner, CornerRadius::ZERO, Color32::from_rgb(28, 30, 34));
    let w = (inner.width() * fraction.clamp(0.0, 1.0) / 6.0).floor() * 6.0;
    let fill = Rect::from_min_size(inner.min, vec2(w, inner.height()));
    let (c, hi, lo) = if failed {
        (Color32::from_rgb(170, 50, 44), Color32::from_rgb(220, 90, 80), Color32::from_rgb(120, 34, 30))
    } else {
        (Color32::from_rgb(76, 168, 50), Color32::from_rgb(128, 216, 92), Color32::from_rgb(48, 116, 30))
    };
    p.rect_filled(fill, CornerRadius::ZERO, c);
    p.rect_filled(Rect::from_min_size(fill.min, vec2(w, 3.0)), CornerRadius::ZERO, hi);
    p.rect_filled(Rect::from_min_max(pos2(fill.min.x, fill.max.y - 3.0), fill.max), CornerRadius::ZERO, lo);
    let mut x = inner.min.x + 6.0;
    while x < inner.min.x + w {
        p.line_segment([pos2(x, inner.min.y + 3.0), pos2(x, inner.max.y - 3.0)], Stroke::new(1.0, Color32::from_black_alpha(50)));
        x += 6.0;
    }
}

/// A key cap: dark, thin border, mono label.
pub fn keycap(p: &Painter, at: Pos2, label: &str) -> f32 {
    let w = (label.chars().count() as f32 * 9.5 + 22.0).max(46.0);
    let r = Rect::from_min_size(at, vec2(w, 28.0));
    p.rect_filled(r, CornerRadius::ZERO, with_alpha(Color32::WHITE, 0.06));
    p.rect_stroke(r, CornerRadius::ZERO, Stroke::new(1.0, with_alpha(Color32::WHITE, 0.45)), StrokeKind::Inside);
    p.line_segment([pos2(r.min.x + 1.0, r.max.y - 1.0), pos2(r.max.x - 1.0, r.max.y - 1.0)], Stroke::new(2.0, with_alpha(Color32::WHITE, 0.25)));
    p.text(r.center(), Align2::CENTER_CENTER, label, mono(12.0), TEXT);
    w
}

/// The scene behind everything: the backdrop image drifting slowly, police lights breathing at the top edge, a slow
/// scan line.
pub fn background(p: &Painter, rect: Rect, t: f32, image: Option<&egui::TextureHandle>) {
    p.rect_filled(rect, CornerRadius::ZERO, BG);
    if let Some(tex) = image {
        let size = tex.size_vec2();
        let over = size - rect.size();
        let k = (t * 0.035).sin() * 0.5 + 0.5;
        let off = vec2(over.x * k, over.y * (0.3 + 0.4 * (t * 0.027).cos() * 0.5 + 0.2));
        let r = Rect::from_min_size(rect.min - off, size);
        p.image(tex.id(), r, Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0)), Color32::WHITE);
    }
    // red and blue light from above, alternating like a light bar
    let phase = (t * 2.2).sin();
    for (x0, x1, c, a) in [
        (rect.min.x, rect.center().x, POLICE_RED, 0.10 + 0.08 * phase.max(0.0)),
        (rect.center().x, rect.max.x, BLUE, 0.10 + 0.08 * (-phase).max(0.0)),
    ] {
        let mut mesh = egui::Mesh::default();
        let top = with_alpha(c, a);
        let bottom = with_alpha(c, 0.0);
        mesh.colored_vertex(pos2(x0, rect.min.y), top);
        mesh.colored_vertex(pos2(x1, rect.min.y), top);
        mesh.colored_vertex(pos2(x0, rect.min.y + 160.0), bottom);
        mesh.colored_vertex(pos2(x1, rect.min.y + 160.0), bottom);
        mesh.add_triangle(0, 1, 2);
        mesh.add_triangle(1, 3, 2);
        p.add(Shape::mesh(mesh));
    }
    let sy = rect.min.y + ((t * 30.0) % rect.height());
    p.line_segment([pos2(rect.min.x, sy), pos2(rect.max.x, sy)], Stroke::new(1.0, with_alpha(Color32::WHITE, 0.025)));
}

/// A thin segmented ring turning: in progress.
pub fn spinner(p: &Painter, c: Pos2, t: f32, color: Color32) {
    for i in 0..10 {
        let a = i as f32 / 10.0 * std::f32::consts::TAU + t * 4.0;
        let alpha = (i as f32 + 1.0) / 10.0;
        let d = vec2(a.cos(), a.sin());
        p.line_segment([c + d * 5.0, c + d * 9.0], Stroke::new(2.0, with_alpha(color, alpha)));
    }
}

/// A status lamp: lit or dark.
pub fn lamp(p: &Painter, c: Pos2, color: Color32, lit: bool) {
    if lit {
        p.circle_filled(c, 7.0, with_alpha(color, 0.25));
        p.circle_filled(c, 3.5, color);
    } else {
        p.circle_stroke(c, 3.5, Stroke::new(1.0, FAINT));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Minecraft's pixel blocks (the hotbar, the loadout tiles)

#[derive(Clone, Copy)]
pub enum Block {
    Grass,
    Tnt,
    Brick,
}

fn rgb(c: u32) -> Color32 {
    Color32::from_rgb((c >> 16) as u8, (c >> 8) as u8, c as u8)
}

fn texture(b: Block, top: bool, x: usize, y: usize, salt: u32) -> Color32 {
    let n = hash(x as u32 * 7 + y as u32 * 13 + salt) % 4;
    match b {
        Block::Grass => {
            if top || y == 0 {
                rgb([0x5da034, 0x6ab03c, 0x54922e, 0x76ba46][n as usize])
            } else {
                rgb([0x866043, 0x79553a, 0x966c4a, 0x6c4c34][n as usize])
            }
        }
        Block::Tnt => {
            if top {
                if (x == 1 || x == 2) && (y == 1 || y == 2) { rgb(0x2a2a2a) } else { rgb(0xdb4a3c) }
            } else if y == 1 || y == 2 {
                if n == 0 { rgb(0x1e1e1e) } else { rgb(0xeeeeee) }
            } else {
                rgb([0xdb4a3c, 0xc23a2e, 0xe0584a, 0xb8352a][n as usize])
            }
        }
        Block::Brick => {
            if y % 2 == 1 && (x + y / 2) % 2 == 0 {
                rgb(0xb0a49a)
            } else {
                rgb([0x9a4a3a, 0x8c4234, 0xa65444, 0x84402f][n as usize])
            }
        }
    }
}

pub fn hash(mut x: u32) -> u32 {
    x ^= x >> 16;
    x = x.wrapping_mul(0x7feb352d);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846ca68b);
    x ^ (x >> 16)
}

fn shade(c: Color32, f: f32, alpha: f32) -> Color32 {
    Color32::from_rgba_unmultiplied((c.r() as f32 * f) as u8, (c.g() as f32 * f) as u8, (c.b() as f32 * f) as u8, (alpha * 255.0) as u8)
}

/// An isometric cube centred at `c` (`size` = its width), each face a 4x4 grid of texture pixels.
pub fn cube(p: &Painter, c: Pos2, size: f32, b: Block, alpha: f32, salt: u32) {
    let w = size / 2.0;
    let h = size / 4.0;
    let e = size / 2.0;
    let top = c + vec2(0.0, -h - e / 2.0);
    let left = top + vec2(-w, h);
    let right = top + vec2(w, h);
    let front = top + vec2(0.0, 2.0 * h);
    let quad = |a: Pos2, u: egui::Vec2, v: egui::Vec2, i: usize, j: usize| -> Vec<Pos2> {
        let s = 0.25;
        let o = a + u * (i as f32 * s) + v * (j as f32 * s);
        vec![o, o + u * s, o + u * s + v * s, o + v * s]
    };
    let mut shapes = Vec::with_capacity(48);
    for j in 0..4 {
        for i in 0..4 {
            shapes.push(Shape::convex_polygon(quad(left, top - left, front - left, i, j), shade(texture(b, true, i, j, salt), 1.0, alpha), Stroke::NONE));
            shapes.push(Shape::convex_polygon(quad(left, front - left, vec2(0.0, e), i, j), shade(texture(b, false, i, j, salt + 1), 0.82, alpha), Stroke::NONE));
            shapes.push(Shape::convex_polygon(quad(front, right - front, vec2(0.0, e), i, j), shade(texture(b, false, i, j, salt + 2), 0.62, alpha), Stroke::NONE));
        }
    }
    p.extend(shapes);
}

/// A thin grass-and-dirt pixel line along the bottom edge: the Minecraft ground, in miniature.
pub fn ground_line(p: &Painter, rect: Rect) {
    let px = 4.0;
    let cols = (rect.width() / px).ceil() as u32;
    let rows = (rect.height() / px).ceil() as u32;
    for y in 0..rows {
        for x in 0..cols {
            let n = hash(x * 31 + y * 977 + 5) % 4;
            let c = if y == 0 {
                rgb([0x5da034, 0x6ab03c, 0x54922e, 0x76ba46][n as usize])
            } else {
                rgb([0x866043, 0x79553a, 0x966c4a, 0x6c4c34][n as usize])
            };
            let r = Rect::from_min_size(rect.min + vec2(x as f32 * px, y as f32 * px), vec2(px, px)).intersect(rect);
            p.rect_filled(r, CornerRadius::ZERO, shade(c, 0.85, 1.0));
        }
    }
}
