/*
 * Xournal++
 *
 * Toolbar buttons for favourite pens: each button remembers a pen or highlighter (tool, colour, size, line style and
 * fill) and switches to it when clicked. They can be put into any toolbar, including the floating toolbox, which a
 * stylus button can open next to the pen.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <array>     // for array
#include <optional>  // for optional
#include <string>    // for string
#include <vector>    // for vector

#include <gtk/gtk.h>  // for GtkWidget

#include "control/ToolEnums.h"  // for ToolType, ToolSize
#include "util/Color.h"         // for Color

#include "AbstractToolItem.h"  // for AbstractToolItem

class Control;

struct FavoritePen {
    ToolType tool = TOOL_PEN;  ///< TOOL_PEN or TOOL_HIGHLIGHTER
    Color color{};
    ToolSize size = TOOL_SIZE_MEDIUM;
    std::string lineStyle = "plain";  ///< as StrokeStyle::formatStyle (pen only)
    bool fill = false;
    int fillAlpha = 128;
};

namespace FavoritePens {
constexpr size_t COUNT = 8;
using Slots = std::array<std::optional<FavoritePen>, COUNT>;

/// Format: one entry per slot separated by '|', each empty or "tool;AARRGGBB;size;lineStyle;fill;fillAlpha"
Slots parse(const std::string& s);
std::string format(const Slots& slots);
}  // namespace FavoritePens

class FavoritePenToolItem: public AbstractToolItem {
public:
    FavoritePenToolItem(Control* control, size_t slot);
    ~FavoritePenToolItem() override;

    xoj::util::WidgetSPtr createItem(bool horizontal) override;
    std::string getToolDisplayName() const override;
    GtkWidget* getNewToolIcon() const override;

private:
    std::optional<FavoritePen> get() const;
    void set(const std::optional<FavoritePen>& pen);

    /// Switches to the favourite pen (or, for an empty slot, saves the current pen into it)
    void activate();
    /// Saves the current pen or highlighter into this slot
    void saveCurrent();
    void showMenu(GtkWidget* button);
    void refreshButtons();
    std::string tooltip() const;

    static void onClicked(GtkButton* button, FavoritePenToolItem* self);
    static gboolean onButtonPress(GtkWidget* button, GdkEventButton* event, FavoritePenToolItem* self);
    static void onLongPress(GtkGestureLongPress* gesture, gdouble x, gdouble y, FavoritePenToolItem* self);
    static void onButtonDestroyed(gpointer self, GObject* button);

    Control* control;
    size_t slot;
    std::vector<GtkWidget*> buttons;  ///< Buttons created for this slot (not owned)
    bool suppressNextClick = false;   ///< A long press opened the menu: ignore the click that follows
};
