/*
 * Xournal++
 *
 * Displays a single page
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <memory>   // for unique_ptr, shared_ptr
#include <mutex>    // for mutex
#include <string>   // for string
#include <vector>   // for vector

#include <cairo.h>    // for cairo_t
#include <gdk/gdk.h>  // for GdkEventKey, GdkRGBA, GdkRectangle
#include <gtk/gtk.h>  // for GtkWidget

#include "gui/inputdevices/DeviceId.h"
#include "gui/inputdevices/InputEvents.h"
#include "model/PageListener.h"       // for PageListener
#include "model/PageRef.h"            // for PageRef
#include "util/Rectangle.h"           // for Rectangle
#include "util/raii/CairoWrappers.h"  // for CairoSurfaceSPtr
#include "util/raii/GSourceURef.h"    // for GSourceURef
#include "view/Mask.h"                // for Mask
#include "view/Repaintable.h"         // for Repaintable

#include "Layout.h"            // for Layout
#include "LegacyRedrawable.h"  // for LegacyRedrawable

class EraseHandler;
class ImageSizeSelection;
class InputHandler;
class LaserPointerHandler;
class SearchControl;
class Selector;
class Settings;
class Text;
class TextEditor;
class VerticalToolHandler;
class XournalView;
class Element;
class PositionInputData;
class Range;
class TexImage;
class XojPdfRectangle;
class XojPdfPage;

namespace xoj::view {
class OverlayView;
class ToolView;
}  // namespace xoj::view

class XojPageView: public LegacyRedrawable, public PageListener, public xoj::view::Repaintable {
public:
    XojPageView(XournalView* xournal, const PageRef& page);
    ~XojPageView() override;

public:
    void addOverlayView(std::unique_ptr<xoj::view::OverlayView>);
    void rerenderPage(bool sizeChanged = false) override;
    void rerenderRect(double x, double y, double width, double height) override;

    void repaintPage() const override;
    void repaintArea(double x1, double y1, double x2, double y2) const override;

    // Repaintable interface
    void flagDirtyRegion(const Range& rg) const override;
    /**
     * @brief This draws the ToolView directly onto the buffer, deletes the view and repaints the given range
     *      Used to avoid blinking when a tool finished editing an element
     */
    void drawAndDeleteToolView(xoj::view::ToolView* v, const Range& rg) override;
    /**
     * @brief Simply deletes an overlay and any trace of it on the display (provided the overlay is contained in the
     * given range)
     */
    void deleteOverlayView(xoj::view::OverlayView* v, const Range& rg) override;

    double getZoom() const override;
    ZoomControl* getZoomControl() const override;
    Range getVisiblePart() const override;

    double getWidth() const override;
    double getHeight() const override;
    // End of Repaintable interface

    xoj::util::Rectangle<double> toWindowCoordinates(const xoj::util::Rectangle<double>& r) const override;


    void setSelected(bool selected);

    void setIsVisible(bool visible);

    bool isSelected() const;
    inline bool isVisible() const { return visible; }

    void endText();

    void endSpline();

    bool searchTextOnPage(const std::string& text, size_t index, size_t* occurrences, XojPdfRectangle* matchRect);

    bool onKeyPressEvent(const KeyEvent& event);
    bool onKeyReleaseEvent(const KeyEvent& event);

    bool cut();
    bool copy();
    bool paste();

    bool actionDelete();

    void deleteViewBuffer() override;

    /**
     * Returns whether this PageView contains the
     * given point on the display
     */
    bool containsPoint(int x, int y, bool local = false) const;

    /**
     * Returns Row assigned in current layout
     */
    int getMappedRow() const;

    /**
     * Returns Column assigned in current layout
     */
    int getMappedCol() const;

    GdkRGBA getSelectionColor() override;
    bool hasBuffer() const;

    TextEditor* getTextEditor();

    /**
     * Returns a reference to the XojPage belonging to
     * this PageView
     */
    const PageRef getPage() const;

    XournalView* getXournal() const;

    /**
     * Returns the width of this XojPageView as displayed
     * on the display taking into account the current zoom
     */
    int getDisplayWidth() const;
    double getDisplayWidthDouble() const;
    /**
     * Returns the height of this XojPageView as displayed
     * on the display taking into account the current zoom
     */
    int getDisplayHeight() const;
    double getDisplayHeightDouble() const;

    /**
     * Returns the x coordinate of this XojPageView with
     * respect to the display
     */
    int getX() const override;

    /**
     * Returns the y coordinate of this XojPageView with
     * respect to the display
     */
    int getY() const override;

    const TexImage* getSelectedTex() const;
    const Text* getSelectedText() const;

    xoj::util::Rectangle<double> getRect() const;

public:  // event handler
    bool onButtonPressEvent(const PositionInputData& pos);
    bool onButtonReleaseEvent(const PositionInputData& pos);
    bool onButtonDoublePressEvent(const PositionInputData& pos);
    bool onButtonTriplePressEvent(const PositionInputData& pos);
    bool onMotionNotifyEvent(const PositionInputData& pos);
    void onSequenceCancelEvent(DeviceId id);
    void onTapEvent(const PositionInputData& pos);

    /**
     * This event fires after onButtonPressEvent and also
     * if no input sequence is actively running and a stylus button was pressed
     */
    bool onButtonClickEvent(const PositionInputData& pos);

    /**
     * This method actually repaints the XojPageView, triggering
     * a rerender call if necessary
     */
    bool paintPage(cairo_t* cr, GdkRectangle* rect);

    void deleteLaserPointerHandler();

public:  // listener
    void rectChanged(xoj::util::Rectangle<double>& rect) override;
    void rangeChanged(Range& range) override;
    void pageChanged() override;
    void elementChanged(const Element* elem) override;
    void elementsChanged(const std::vector<const Element*>& elements, const Range& range) override;

private:
    void startText(double x, double y);

    void drawLoadingPage(cairo_t* cr);

    /**
     * @brief Make and display a popover dialog near the given location.
     *
     * @param rect specifies the location of the dialog.
     * @param child is added to the dialog before displaying.
     * @returns a pointer to the popover dialog.
     */
    GtkWidget* makePopover(const XojPdfRectangle& rect, GtkWidget* child);

    /**
     * @brief Display a popover with link-related actions, if one
     *  is at a given location on the page.
     *
     * @param page is the current page
     * @param targetX
     * @param targetY the link must contain (targetX, targetY)
     * @returns true iff a URI link exists near/at (targetX, targetY) => a popover was shown
     */
    bool displayLinkPopover(std::shared_ptr<XojPdfPage> page, double targetX, double targetY);

    void setX(int x);
    void setY(int y);

    void setMappedRowCol(int row, int col);  // row, column assigned by mapper during layout.

public:
    /**
     * Shows the floating toolbox at the location of an input event
     */
    void showFloatingToolbox(const PositionInputData& pos);

private:
    /**
     * Shows the PDF toolbox at the location of an input event
     */
    void showPdfToolbox(const PositionInputData& pos);

    void deleteView(xoj::view::OverlayView* v);

private:
    PageRef page;
    XournalView* xournal = nullptr;
    Settings* settings = nullptr;
    std::unique_ptr<EraseHandler> eraser;
    std::unique_ptr<InputHandler> inputHandler;

    std::vector<std::unique_ptr<xoj::view::OverlayView>> overlayViews;

    /**
     * The selected (while selection)
     */
    std::unique_ptr<Selector> selector;

    /**
     * The text editor
     */
    std::unique_ptr<TextEditor> textEditor;

    /**
     * For image insertion with size (selects the size)
     */
    std::unique_ptr<ImageSizeSelection> imageSizeSelection;

    std::unique_ptr<LaserPointerHandler> laserPointer;

    /**
     * For keeping old text changes to undo!
     */
    Text* oldtext;

    bool visible = true;
    bool selected = false;

    xoj::view::Mask buffer;
    /// The part of the page drawn in the buffer (guarded by drawingMutex)
    Range bufferRange;
    std::mutex drawingMutex;

    bool inEraser = false;

    /**
     * Vertical Space
     */
    std::unique_ptr<VerticalToolHandler> verticalSpace;

    /**
     * Search handling
     */
    std::unique_ptr<SearchControl> search;

    std::mutex repaintRectMutex;
    std::vector<xoj::util::Rectangle<double>> rerenderRects;
    bool rerenderComplete = false;
    bool sizeChanged = false;
    /// The part of the page the pending complete rerender draws (guarded by repaintRectMutex)
    Range pendingRenderWindow;

    /**
     * Tall pages are not drawn whole into the buffer, which would take too much memory (or exceed cairo's maximum
     * surface size): only the visible part and about one screen above and below are. The buffer is drawn again around
     * the visible part when scrolling gets close to its edge.
     */
    /// The part of the page a complete rerender should draw now (call from the UI thread)
    Range computeRenderWindow() const;
    /// Whether the buffer no longer covers enough around the visible part (call from the UI thread, drawingMutex held)
    bool bufferNeedsRecentering() const;
    /// Grows an endless page if its content gets close to the bottom (after the current change is complete)
    void growIfEndless(double bottom);
    /// Pages up to this height (in pixels at the current zoom) are always drawn whole
    static constexpr double MAX_WHOLE_PAGE_RENDER_HEIGHT = 4096;

    int dispX{};  // position on display - set in Layout::layoutPages
    int dispY{};


    int mappedRow{};
    int mappedCol{};

    DeviceId currentSequenceDeviceId;


    /// Animated GIFs: while the page is visible, a timer shows their next frames (started when the page is painted)
    void scheduleAnimation();
    static void onAnimationTimer(XojPageView* self);
    /// Shows the frames due now; returns the milliseconds until the next one (-1: no animated images)
    int advanceAnimations(bool redraw);
    /// Declared last: destroyed first, so the timer can't fire on a half-destroyed page view
    xoj::util::GSourceURef animationTimer;

    friend class RenderJob;
    friend class InputHandler;
    friend class BaseSelectObject;
    friend class SelectObject;
    friend class PlayObject;
    friend class PdfFloatingToolbox;
    // only function allowed to setX(), setY(), setMappedRowCol():
    friend void Layout::layoutPages(int width, int height);
};
