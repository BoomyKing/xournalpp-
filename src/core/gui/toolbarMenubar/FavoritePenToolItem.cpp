#include "FavoritePenToolItem.h"

#include <algorithm>  // for clamp, remove
#include <cinttypes>  // for PRIx32
#include <cmath>      // for M_PI
#include <cstdio>     // for snprintf
#include <sstream>    // for istringstream
#include <utility>    // for move

#include "control/Control.h"                 // for Control
#include "control/Tool.h"                    // for Tool
#include "control/ToolHandler.h"             // for ToolHandler
#include "control/actions/ActionDatabase.h"  // for ActionDatabase
#include "control/settings/Settings.h"       // for Settings
#include "enums/Action.enum.h"               // for Action
#include "gui/FloatingToolbox.h"             // for FloatingToolbox
#include "gui/MainWindow.h"
#include "gui/toolbarMenubar/model/ColorPalette.h"  // for Palette
#include "model/StrokeStyle.h"                      // for formatStyle
#include "util/Color.h"                             // for cairo_set_source_rgbi
#include "util/NamedColor.h"                        // for NamedColor
#include "util/i18n.h"                              // for _, FS, _F
#include "util/raii/CStringWrapper.h"               // for OwnedCString                  // for MainWindow
#include "util/raii/CairoWrappers.h"                // for CairoSPtr
#include "util/raii/GObjectSPtr.h"                  // for GObjectSPtr

namespace FavoritePens {
static auto split(const std::string& s, char sep) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(s);
    while (std::getline(in, part, sep)) {
        parts.push_back(part);
    }
    if (!s.empty() && s.back() == sep) {
        parts.emplace_back();
    }
    return parts;
}

auto parse(const std::string& s) -> Slots {
    Slots slots;
    auto entries = split(s, '|');
    for (size_t i = 0; i < entries.size() && i < COUNT; i++) {
        auto f = split(entries[i], ';');
        if (f.size() < 6) {
            continue;
        }
        try {
            FavoritePen pen;
            pen.tool = f[0] == "highlighter" ? TOOL_HIGHLIGHTER : TOOL_PEN;
            pen.color = Color(static_cast<uint32_t>(std::stoul(f[1], nullptr, 16)));
            int size = std::stoi(f[2]);
            if (size < TOOL_SIZE_VERY_FINE || size > TOOL_SIZE_VERY_THICK) {
                continue;
            }
            pen.size = static_cast<ToolSize>(size);
            pen.lineStyle = f[3].empty() ? "plain" : f[3];
            pen.fill = f[4] == "1";
            pen.fillAlpha = std::clamp(std::stoi(f[5]), 0, 255);
            slots[i] = pen;
        } catch (const std::exception&) {
            // ignore broken entries
        }
    }
    return slots;
}

auto format(const Slots& slots) -> std::string {
    std::string s;
    for (size_t i = 0; i < COUNT; i++) {
        if (i > 0) {
            s += '|';
        }
        if (const auto& pen = slots[i]) {
            char color[16];
            std::snprintf(color, sizeof(color), "%08" PRIx32, static_cast<uint32_t>(pen->color));
            s += std::string(pen->tool == TOOL_HIGHLIGHTER ? "highlighter" : "pen") + ";" + color + ";" +
                 std::to_string(static_cast<int>(pen->size)) + ";" + pen->lineStyle + ";" + (pen->fill ? "1" : "0") +
                 ";" + std::to_string(pen->fillAlpha);
        }
    }
    return s;
}
}  // namespace FavoritePens

namespace {
constexpr int ICON_SIZE = 22;

/// Draws the favourite pen: a dot for a pen (bigger for thicker sizes), a marker tip for a highlighter, a dashed "+"
/// circle for an empty slot
auto newIcon(const std::optional<FavoritePen>& pen) -> GtkWidget* {
    xoj::util::CairoSurfaceSPtr buf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE),
                                    xoj::util::adopt);
    xoj::util::CairoSPtr crPtr(cairo_create(buf.get()), xoj::util::adopt);
    cairo_t* cr = crPtr.get();
    const double mid = ICON_SIZE / 2.0;

    if (!pen) {
        const double dash[] = {2.0, 2.0};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 1);
        cairo_set_line_width(cr, 1);
        cairo_arc(cr, mid, mid, mid - 2, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        cairo_move_to(cr, mid, mid - 4);
        cairo_line_to(cr, mid, mid + 4);
        cairo_move_to(cr, mid - 4, mid);
        cairo_line_to(cr, mid + 4, mid);
        cairo_stroke(cr);
    } else {
        const double s = static_cast<double>(pen->size);  // 0 (very fine) to 4 (very thick)
        if (pen->tool == TOOL_HIGHLIGHTER) {
            const double h = 6 + 2.5 * s;
            cairo_rectangle(cr, 3, mid - h / 2, ICON_SIZE - 6, h);
            Util::cairo_set_source_rgbi(cr, pen->color, 0.6);
            cairo_fill_preserve(cr);
        } else {
            const double r = 3 + 1.8 * s;
            cairo_arc(cr, mid, mid, r, 0, 2 * M_PI);
            Util::cairo_set_source_rgbi(cr, pen->color, 1);
            cairo_fill_preserve(cr);
        }
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (pen->tool == TOOL_PEN && pen->lineStyle != "plain") {
            // dashed ring for dashed / dotted pens
            const double dash[] = {2.0, 2.0};
            cairo_set_dash(cr, dash, 2, 0);
            cairo_arc(cr, mid, mid, mid - 1, 0, 2 * M_PI);
            cairo_stroke(cr);
        }
    }

    xoj::util::GObjectSPtr<GdkPixbuf> pixbuf(gdk_pixbuf_get_from_surface(buf.get(), 0, 0, ICON_SIZE, ICON_SIZE),
                                             xoj::util::adopt);
    GtkWidget* img = gtk_image_new_from_pixbuf(pixbuf.get());
    gtk_widget_show(img);
    return img;
}

auto sizeName(ToolSize size) -> std::string {
    switch (size) {
        case TOOL_SIZE_VERY_FINE:
            return _("very fine");
        case TOOL_SIZE_FINE:
            return _("fine");
        case TOOL_SIZE_MEDIUM:
            return _("medium");
        case TOOL_SIZE_THICK:
            return _("thick");
        case TOOL_SIZE_VERY_THICK:
            return _("very thick");
        default:
            return "";
    }
}
}  // namespace

FavoritePenToolItem::FavoritePenToolItem(Control* control, size_t slot):
        AbstractToolItem("FAVORITE_PEN_" + std::to_string(slot + 1), Category::TOOLS), control(control), slot(slot) {}

FavoritePenToolItem::~FavoritePenToolItem() {
    for (GtkWidget* b: buttons) {
        g_object_weak_unref(G_OBJECT(b), onButtonDestroyed, this);
    }
}

auto FavoritePenToolItem::get() const -> std::optional<FavoritePen> {
    return FavoritePens::parse(control->getSettings()->getFavoritePens())[slot];
}

void FavoritePenToolItem::set(const std::optional<FavoritePen>& pen) {
    auto slots = FavoritePens::parse(control->getSettings()->getFavoritePens());
    slots[slot] = pen;
    control->getSettings()->setFavoritePens(FavoritePens::format(slots));
    refreshButtons();
}

auto FavoritePenToolItem::getToolDisplayName() const -> std::string { return FS(_F("Favourite pen {1}") % (slot + 1)); }

auto FavoritePenToolItem::getNewToolIcon() const -> GtkWidget* { return newIcon(get()); }

auto FavoritePenToolItem::tooltip() const -> std::string {
    auto pen = get();
    if (!pen) {
        return FS(_F("<b>Favourite pen {1}</b>\nEmpty: tap to save the current pen here") % (slot + 1));
    }
    // colour: its name in the palette if it has one, its hex code otherwise
    const auto rgb = static_cast<uint32_t>(pen->color) & 0xffffffU;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%06" PRIx32, rgb);
    std::string colorName = hex;
    const Palette& palette = control->getPalette();
    for (size_t i = 0; i < palette.size(); i++) {
        const NamedColor& c = palette.getColorAt(i);
        if ((static_cast<uint32_t>(c.getColor()) & 0xffffffU) == rgb) {
            colorName = c.getName();
            break;
        }
    }
    std::string style;
    if (pen->tool == TOOL_PEN && pen->lineStyle != "plain") {
        style = pen->lineStyle == "dash"    ? _("dashed") :
                pen->lineStyle == "dashdot" ? _("dash-dotted") :
                pen->lineStyle == "dot"     ? _("dotted") :
                                              _("custom dashes");
    }
    auto escapedName = xoj::util::OwnedCString::assumeOwnership(g_markup_escape_text(colorName.c_str(), -1));
    std::string text = "<b>" + std::string(pen->tool == TOOL_HIGHLIGHTER ? _("Highlighter") : _("Pen")) + "</b>\n";
    text += std::string("<span foreground=\"") + hex + "\">⬤</span>  " + escapedName.get() + "\n";
    text += FS(_F("Size: {1}") % sizeName(pen->size));
    if (!style.empty()) {
        text += "\n" + FS(_F("Line: {1}") % style);
    }
    if (pen->fill) {
        text += std::string("\n") + _("Filled shapes");
    }
    text += std::string("\n<small>") + _("Long-press or right-click to change") + "</small>";
    return text;
}

auto FavoritePenToolItem::createItem(bool) -> xoj::util::WidgetSPtr {
    GtkWidget* btn = gtk_button_new();
    gtk_widget_set_can_focus(btn, false);
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_button_set_image(GTK_BUTTON(btn), newIcon(get()));
    g_signal_connect(btn, "clicked", G_CALLBACK(onClicked), this);

    // A card describing the pen, shown as soon as the pointer (or a hovering stylus) is over the button: regular
    // tooltips need the pointer to rest still for a while, which is hard with a stylus
    GtkWidget* info = gtk_popover_new(btn);
    gtk_popover_set_modal(GTK_POPOVER(info), false);
    gtk_popover_set_position(GTK_POPOVER(info), GTK_POS_BOTTOM);
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);
    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 6);
    gtk_container_add(GTK_CONTAINER(info), label);
    gtk_widget_show(label);
    g_object_set_data(G_OBJECT(btn), "favorite-info", info);
    g_object_set_data(G_OBJECT(btn), "favorite-info-label", label);
    gtk_widget_add_events(btn, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(btn, "enter-notify-event", G_CALLBACK(+[](GtkWidget* b, GdkEvent*, FavoritePenToolItem* self) {
                         self->showInfo(b, true);
                         return gboolean(false);
                     }),
                     this);
    g_signal_connect(btn, "leave-notify-event", G_CALLBACK(+[](GtkWidget* b, GdkEvent* e, FavoritePenToolItem* self) {
                         if (e->crossing.mode != GDK_CROSSING_NORMAL) {
                             return gboolean(false);  // caused by the card itself appearing, not by the pointer
                         }
                         self->showInfo(b, false);
                         return gboolean(false);
                     }),
                     this);
    g_signal_connect(btn, "button-press-event", G_CALLBACK(onButtonPress), this);

    GtkGesture* longPress = gtk_gesture_long_press_new(btn);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(longPress), GTK_PHASE_CAPTURE);
    g_signal_connect(longPress, "pressed", G_CALLBACK(onLongPress), this);
    // the gesture lives as long as the button
    g_object_set_data_full(G_OBJECT(btn), "favorite-long-press", longPress, g_object_unref);

    buttons.push_back(btn);
    g_object_weak_ref(G_OBJECT(btn), onButtonDestroyed, this);

    GtkToolItem* it = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(it), btn);
    return xoj::util::WidgetSPtr(GTK_WIDGET(it), xoj::util::adopt);
}

void FavoritePenToolItem::onButtonDestroyed(gpointer self, GObject* button) {
    auto& v = static_cast<FavoritePenToolItem*>(self)->buttons;
    v.erase(std::remove(v.begin(), v.end(), reinterpret_cast<GtkWidget*>(button)), v.end());
}

void FavoritePenToolItem::refreshButtons() {
    for (GtkWidget* b: buttons) {
        gtk_button_set_image(GTK_BUTTON(b), newIcon(get()));
    }
}

void FavoritePenToolItem::showInfo(GtkWidget* button, bool show) {
    auto* info = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(button), "favorite-info"));
    auto* label = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(button), "favorite-info-label"));
    if (!info || !label) {
        return;
    }
    if (show) {
        gtk_label_set_markup(GTK_LABEL(label), tooltip().c_str());
        gtk_widget_show(info);
    } else {
        gtk_widget_hide(info);
    }
}

void FavoritePenToolItem::onClicked(GtkButton*, FavoritePenToolItem* self) {
    if (self->suppressNextClick) {
        self->suppressNextClick = false;
        return;
    }
    self->activate();
}

auto FavoritePenToolItem::onButtonPress(GtkWidget* button, GdkEventButton* event,
                                        FavoritePenToolItem* self) -> gboolean {
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        self->showMenu(button);
        return true;
    }
    return false;
}

void FavoritePenToolItem::onLongPress(GtkGestureLongPress* gesture, gdouble, gdouble, FavoritePenToolItem* self) {
    GtkWidget* button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    self->suppressNextClick = true;
    self->showMenu(button);
}

void FavoritePenToolItem::saveCurrent() {
    ToolHandler* th = control->getToolHandler();
    const Tool* tool = th->getActiveTool();
    if (tool->getToolType() != TOOL_PEN && tool->getToolType() != TOOL_HIGHLIGHTER) {
        tool = &th->getTool(TOOL_PEN);
    }
    FavoritePen pen;
    pen.tool = tool->getToolType();
    pen.color = tool->getColor();
    pen.size = tool->getSize() == TOOL_SIZE_NONE ? TOOL_SIZE_MEDIUM : tool->getSize();
    pen.lineStyle = pen.tool == TOOL_PEN ? StrokeStyle::formatStyle(tool->getLineStyle()) : "plain";
    if (pen.lineStyle.empty() || pen.lineStyle.find_first_of("|;") != std::string::npos) {
        pen.lineStyle = "plain";
    }
    pen.fill = tool->getFill();
    pen.fillAlpha = tool->getFillAlpha();
    set(pen);
}

void FavoritePenToolItem::activate() {
    for (GtkWidget* b: buttons) {
        showInfo(b, false);
    }
    auto pen = get();
    if (!pen) {
        saveCurrent();
        return;
    }
    control->clearSelection();
    ActionDatabase* db = control->getActionDatabase();
    db->fireChangeActionState(Action::SELECT_TOOL, pen->tool);
    db->fireChangeActionState(Action::TOOL_COLOR, pen->color);
    db->fireChangeActionState(Action::TOOL_SIZE, pen->size);
    if (pen->tool == TOOL_PEN) {
        db->fireChangeActionState(Action::TOOL_PEN_LINE_STYLE, pen->lineStyle.c_str());
    }
    db->fireChangeActionState(Action::TOOL_FILL, pen->fill);
    if (pen->tool == TOOL_PEN) {
        control->getToolHandler()->setPenFill(pen->fillAlpha);
    } else {
        control->getToolHandler()->setHighlighterFill(pen->fillAlpha);
    }

    // a favourite picked from the floating toolbox closes it
    if (MainWindow* win = control->getWindow(); win && win->getFloatingToolbox()) {
        win->getFloatingToolbox()->hide();
    }
}

void FavoritePenToolItem::showMenu(GtkWidget* button) {
    showInfo(button, false);
    GtkWidget* menu = gtk_menu_new();
    GtkWidget* save = gtk_menu_item_new_with_label(_("Save current pen here"));
    g_signal_connect_swapped(save, "activate", G_CALLBACK(+[](FavoritePenToolItem* self) { self->saveCurrent(); }),
                             this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), save);
    if (get()) {
        GtkWidget* clear = gtk_menu_item_new_with_label(_("Clear"));
        g_signal_connect_swapped(clear, "activate",
                                 G_CALLBACK(+[](FavoritePenToolItem* self) { self->set(std::nullopt); }), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), clear);
    }
    gtk_widget_show_all(menu);
    // the menu destroys itself once closed
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkWidget* m) {
                         g_idle_add(
                                 +[](gpointer w) -> gboolean {
                                     gtk_widget_destroy(GTK_WIDGET(w));
                                     return G_SOURCE_REMOVE;
                                 },
                                 m);
                     }),
                     nullptr);
    gtk_menu_attach_to_widget(GTK_MENU(menu), button, nullptr);
    gtk_menu_popup_at_widget(GTK_MENU(menu), button, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}
