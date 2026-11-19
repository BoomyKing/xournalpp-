#include "ImageView.h"

#include <algorithm>  // for min, max
#include <cmath>      // for M_PI_2

#include <cairo.h>             // for cairo_image_surface_get_height, cairo_image...
#include <pango/pangocairo.h>  // for pango_cairo_create_layout

#include "model/Image.h"  // for Image
#include "view/View.h"    // for Context, OPACITY_NO_AUDIO, view

using namespace xoj::view;

ImageView::ImageView(const Image* image): image(image) {}

ImageView::~ImageView() = default;

void ImageView::draw(const Context& ctx) const {
    cairo_t* cr = ctx.cr;

    cairo_save(cr);

    cairo_surface_t* img = image->getImage();
    int width = cairo_image_surface_get_width(img);
    int height = cairo_image_surface_get_height(img);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // with animation states, the picture leaves room for the row of buttons under it
    const auto pic = image->getPictureRect();
    double xFactor = pic.width / width;
    double yFactor = pic.height / height;

    cairo_scale(cr, xFactor, yFactor);

    cairo_set_source_surface(cr, img, pic.x / xFactor, pic.y / yFactor);
    // make images translucent when highlighting elements with audio, as they can not have audio
    if (ctx.fadeOutNonAudio) {
        cairo_paint_with_alpha(cr, OPACITY_NO_AUDIO);
    } else {
        cairo_paint(cr);
    }

    cairo_restore(cr);

    drawButtons(cr);
}

void ImageView::drawButtons(cairo_t* cr) const {
    const auto& states = image->getStates();
    if (states.empty()) {
        return;
    }
    cairo_save(cr);
    const int active = image->getActiveState();
    for (size_t i = 0; i < states.size(); i++) {
        const auto r = image->getButtonRect(i);
        if (r.width <= 0 || r.height <= 0) {
            continue;
        }
        const double radius = std::min(r.height, r.width) * 0.25;
        cairo_new_sub_path(cr);
        cairo_arc(cr, r.x + r.width - radius, r.y + radius, radius, -M_PI_2, 0);
        cairo_arc(cr, r.x + r.width - radius, r.y + r.height - radius, radius, 0, M_PI_2);
        cairo_arc(cr, r.x + radius, r.y + r.height - radius, radius, M_PI_2, M_PI);
        cairo_arc(cr, r.x + radius, r.y + radius, radius, M_PI, 3 * M_PI_2);
        cairo_close_path(cr);
        const bool on = static_cast<int>(i) == active;
        if (on) {
            cairo_set_source_rgb(cr, 0.16, 0.42, 0.85);
        } else {
            cairo_set_source_rgb(cr, 0.93, 0.95, 0.98);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.16, 0.42, 0.85);
        cairo_set_line_width(cr, std::max(0.5, r.height * 0.05));
        cairo_stroke(cr);

        // label, shrunk to fit the button (laid out with Pango, which handles right-to-left text like Hebrew)
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_cairo_context_set_resolution(pango_layout_get_context(layout), 72);
        pango_layout_set_text(layout, states[i].name.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
        double size = r.height * 0.45;
        PangoRectangle ink{}, logical{};
        for (int k = 0; k < 14; k++) {
            pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
            pango_layout_get_extents(layout, &ink, &logical);
            if (logical.width / static_cast<double>(PANGO_SCALE) <= r.width * 0.9) {
                break;
            }
            size *= 0.85;
        }
        pango_font_description_free(font);
        if (on) {
            cairo_set_source_rgb(cr, 1, 1, 1);
        } else {
            cairo_set_source_rgb(cr, 0.12, 0.2, 0.35);
        }
        const double tw = logical.width / static_cast<double>(PANGO_SCALE);
        const double th = logical.height / static_cast<double>(PANGO_SCALE);
        cairo_move_to(cr, r.x + (r.width - tw) / 2 - logical.x / static_cast<double>(PANGO_SCALE),
                      r.y + (r.height - th) / 2 - logical.y / static_cast<double>(PANGO_SCALE));
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
    cairo_restore(cr);
}
