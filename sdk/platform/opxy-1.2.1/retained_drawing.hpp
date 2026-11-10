#pragma once
#include "screen.hpp"
#include "rgba_palette.hpp"
#include "glyph_atlas.hpp"
namespace opxy {
namespace screen {
namespace {
// Standard-layout objects keep the stock screen prefix at byte zero.
// The zero-glyph specialization preserves the existing renderer allocation.
template <u32 N, u32 T, u32 P, u32 Paths, u32 Glyphs, u32 Definitions> struct DrawingObject {
    Base base;
    PathNode *nodes[N];                            // path leaves, or 0 when missing or foreign
    u8 authored_visibility[N];                     // each leaf's authored visibility byte
    Leaf *texts[T > 0 ? T : 1];                    // text leaves, or 0
    char shown[T > 0 ? T : 1][draw::text_capacity]; // each slot's applied text
    u32 palette[P > 0 ? P : 1];                    // packed palette entry IDs, or 0
    i32 delta;                                     // elapsed time of the current frame, Q10 seconds
    u32 geometry[Paths > 0 ? Paths : 1];           // each path's command vector as validated at init
    u32 keys[Paths > 0 ? Paths : 1];               // each path's last applied key
    glyph_detail::State<Glyphs, Definitions> glyph_state;
};
template <u32 N, u32 T, u32 P, u32 Paths> struct DrawingObject<N, T, P, Paths, 0, 0> {
    Base base;
    PathNode *nodes[N];
    u8 authored_visibility[N];
    Leaf *texts[T > 0 ? T : 1];
    char shown[T > 0 ? T : 1][draw::text_capacity];
    u32 palette[P > 0 ? P : 1];
    i32 delta;
    u32 geometry[Paths > 0 ? Paths : 1];
    u32 keys[Paths > 0 ? Paths : 1];
};

template <class Frame> struct RetainedDrawing {
    enum {
        path_count = Frame::path_count,
        point_count = Frame::point_count,
        node_count = Frame::node_count,
        text_count = Frame::text_count,
        palette_count = Frame::palette_count
    };
    typedef DrawingObject<node_count, text_count, palette_count, path_count, Frame::glyph_count,
                          Frame::glyph_definition_count>
        Object;

    static PathKind kind(u32 path) {
        return Frame::cubic                                   ? cubic_curve
               : (Frame::segment_paths & (u32(1) << path)) ? separate_segments
               : (Frame::open_paths & (u32(1) << path))    ? open_polyline
                                                            : closed_polyline;
    }
    // Validate all retained command types and bounds before any coordinate write.
    static u32 geometry(void *raw, u32 path, bool initial = false) {
        const PathNode *node = path_node(raw);
        if (!node)
            return 0;
        const Vector &v = node->commands;
        if (!v.begin || v.begin & 3 || v.end < v.begin || v.capacity < v.end)
            return 0;
        if (v.capacity - v.begin > 16384 || (v.end - v.begin) % 32)
            return 0;
        const Command *command = reinterpret_cast<const Command *>(v.begin);
        if (Frame::quadratic) {
            if (v.capacity - v.begin < 32 * Frame::command_capacity)
                return 0;
            return quadratic_types(command, (v.end - v.begin) / 32, Frame::command_capacity, initial) ? v.begin : 0;
        }
        if (Frame::counted) {
            if (v.capacity - v.begin < 32 * command_count(point_count, kind(path)))
                return 0;
            return counted_types(command, (v.end - v.begin) / 32, point_count, kind(path), initial) ? v.begin : 0;
        }
        if (v.end - v.begin != 32 * command_count(point_count, kind(path)))
            return 0;
        return command_types(command, point_count, kind(path)) ? v.begin : 0;
    }

    // Ordinary paths retain topology. Quadratic command streams change only
    // validated M/L/Q records and end within their original reserved capacity.
    static void apply_path(Object *o, const Frame &frame, u32 p) {
        const typename Frame::path_type &path = frame.paths[p];
        if (path.key == o->keys[p])
            return;
        const u32 begin = geometry(o->nodes[p], p);
        if (!begin || begin != o->geometry[p])
            return;
        Command *command = reinterpret_cast<Command *>(begin);
        const u32 count = path_coordinates(command, path, kind(p));
        if (Frame::quadratic || Frame::counted)
            o->nodes[p]->commands.end = begin + 32 * count;
        invalidate(o->nodes[p]);
        o->keys[p] = path.key;
    }

    static void apply_rgba(Object *o, u32 i, u32 color, draw::Paint mode) {
        PathNode *node = o->nodes[i];
        if (!node || !o->base.svg)
            return;
        bool changed = false;
        if (mode != draw::rgba_fill && rgba_detail::unique(o->nodes, node_count, i, false)) {
            const Vector &v = o->base.svg->strokes;
            changed = rgba_detail::set_color(v.begin, v.end, v.capacity, 20, node->stroke, color);
        }
        if (mode != draw::rgba_stroke && static_cast<__INT16_TYPE__>(node->fill) > 0 &&
            rgba_detail::unique(o->nodes, node_count, i, true)) {
            const Vector &v = o->base.svg->fills;
            changed = rgba_detail::set_color(v.begin, v.end, v.capacity, 4, node->fill, color) || changed;
        }
        if (changed)
            invalidate(node);
    }

    // Each node's matrix: its scale times its turn at (x, y), or an affine
    // node's own matrix.
    static void apply_node(Object *o, const Frame &frame, u32 i) {
        const auto &node = frame.nodes[i];
        if (!o->nodes[i])
            return;
        // Stock path_draw checks this byte before allocating/rasterizing cells.
        reinterpret_cast<u8 *>(o->nodes[i])[92] = node.paint == draw::hidden ? 0 : o->authored_visibility[i];
        if (node.paint == draw::hidden)
            return;
        if (Frame::rgba && node.paint >= draw::rgba_stroke)
            apply_rgba(o, i, node.shade, node.paint);
        else if (node.paint != draw::keep)
            paint(o->nodes[i], o->palette[node.shade], node.paint == draw::fill_and_stroke);
        if (!o->nodes[i])
            return;
        const draw::Matrix m = draw::matrix(node);
        const u32 at[6] = {0, 1, 3, 4, 6, 7};
        const i32 value[6] = {m.a, m.b, m.c, m.d, m.x, m.y};
        set_words(&o->nodes[i]->leaf, at, value);
    }

    static void apply_text(Object *o, const Frame &frame, u32 i) {
        const draw::Text &text = frame.texts[i];
        Leaf *leaf = o->texts[i];
        if (!leaf)
            return;
        const u32 at[6] = {0, 1, 3, 4, 6, 7};
        const i32 value[6] = {text.scale, 0, 0, text.scale, text.x, text.y};
        set_words(leaf, at, value);
        char *shown = o->shown[i];
        u32 n = 0;
        while (n < draw::text_capacity && shown[n] == text.chars[n] && text.chars[n])
            ++n;
        if (n < draw::text_capacity && shown[n] == text.chars[n])
            return;
        for (n = 0; n < draw::text_capacity; ++n)
            shown[n] = text.chars[n];
        set_text(leaf, shown);
    }

    static void bind(Object *o, const char *const *ids) {
        for (u32 i = 0; i < node_count; ++i) {
            o->nodes[i] = path_node(find(&o->base, ids[i]));
            o->authored_visibility[i] = o->nodes[i] ? reinterpret_cast<u8 *>(o->nodes[i])[92] : 0;
        }
        for (u32 i = 0; i < text_count; ++i) {
            o->texts[i] = text_node(find(&o->base, ids[node_count + i]));
            o->shown[i][0] = 1; // apply every slot on the first frame
        }
        for (u32 i = 0; i < palette_count; ++i)
            o->palette[i] = palette_ids(find(&o->base, ids[node_count + text_count + i]), o->base.svg);
        for (u32 i = 0; i < path_count; ++i) {
            o->geometry[i] = geometry(o->nodes[i], i, true);
            o->keys[i] = 0;
        }
    }
    // A glyph atlas binds its sources by the machine's glyph_ids, after bind.
    template <class Machine> static void bind_glyphs(Object *o) {
        glyph_detail::Binder<Frame>::template init<Machine>(o);
    }
    // False, applying nothing, when the frame or its glyph atlas is rejected.
    static bool apply(Object *o, const Frame &frame) {
        if (!draw::valid(frame))
            return false;
        if (!glyph_detail::Binder<Frame>::apply(o, frame))
            return false;
        for (u32 p = 0; p < path_count; ++p)
            apply_path(o, frame, p);
        for (u32 i = 0; i < node_count; ++i)
            apply_node(o, frame, i);
        for (u32 i = 0; i < text_count; ++i)
            apply_text(o, frame, i);
        return true;
    }
};
} // namespace
} // namespace screen
} // namespace opxy
