/*
 * Xournal++
 *
 * An Image on the document
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <atomic>   // for atomic
#include <cstddef>  // for size_t
#include <cstdint>  // for int64_t
#include <memory>
#include <optional>     // for optional
#include <string>       // for string
#include <string_view>  // for string_view
#include <utility>      // for pair, make_pair
#include <vector>       // for vector

#include <cairo.h>                  // for cairo_surface_t, cairo_status_t
#include <gdk-pixbuf/gdk-pixbuf.h>  // for GdkPixbufFormat, GdkPixbuf

#include "util/Rectangle.h"  // for Rectangle

#include "Element.h"  // for Element

class ObjectInputStream;
class ObjectOutputStream;


/// A named part of an animated GIF that a button under it plays (see Image::getStates)
struct AnimationState {
    std::string name;
    size_t first = 0;   ///< first frame
    size_t last = 0;    ///< last frame
    bool loop = false;  ///< keep playing (e.g. a motion) instead of stopping at the last frame (e.g. a transformation)
};

/// The decoded frames of an animated GIF: written by the decoding thread, then only read once ready
struct AnimationFrames {
    std::vector<cairo_surface_t*> frames;
    std::vector<int> delays;  ///< how long each frame is shown (ms)
    int64_t duration = 0;     ///< the time all frames take (ms)
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> cancelled{false};
    ~AnimationFrames() {
        for (cairo_surface_t* f: frames) {
            cairo_surface_destroy(f);
        }
    }
};

class Image: public Element {
public:
    Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) = delete;
    Image& operator=(Image&&) = delete;
    virtual ~Image();

public:
    void setWidth(double width);
    void setHeight(double height);

    /// Set the image data by copying the data from the provided string_view.
    void setImage(std::string_view data);

    /// Set the image data by moving the data.
    void setImage(std::string&& data);

    /// Set the image data by copying the data from the provided pixbuf.
    ///
    /// \deprecated Pass the raw image data instead.
    ///
    /// FIXME: remove this method. Currently, it is used by Control::clipboardPasteImage.
    [[deprecated]] void setImage(GdkPixbuf* img);

    /// The image is rendered lazily by default; call this method to render it.
    /// Returns std::nullopt on success, an error message on failure
    std::optional<std::string> renderBuffer() const;

    /// Returns the internal surface that contains the rendered image data.
    cairo_surface_t* getImage() const;

    void scale(double x0, double y0, double fx, double fy, double rotation, bool restoreLineWidth) override;
    void rotate(double x0, double y0, double th) override;

    auto clone() const -> ElementPtr override;

    bool hasData() const;

    /// Return a pointer to the raw data. Note that the pointer will be invalidated if the data is changed.
    const unsigned char* getRawData() const;

    /// Return the length of the raw data.
    size_t getRawDataLength() const;

    /// Return the size of the raw image, or (-1, -1) if the image has not been rendered yet.
    std::pair<int, int> getImageSize() const;

    [[maybe_unused]] GdkPixbufFormat* getImageFormat() const;

    /// Whether the raw data is a GIF. GIFs are saved as they are, so that their animation is kept.
    bool isGif() const;

    /// Whether the image is an animated GIF whose frames are ready (known once the image has been rendered)
    bool isAnimated() const;
    /// Whether the frames of an animated GIF are still being decoded
    bool isDecoding() const;

    /// Shows the frame that is due at the given time (as returned by g_get_monotonic_time()).
    /// Returns whether the frame shown changed. To be called from the main thread.
    bool advanceAnimation(int64_t now) const;

    /// Milliseconds until the next frame of an animated image is due
    int msUntilNextFrame(int64_t now) const;

    /**
     * Animations with states: an animated GIF can have named states (parts of its frames), with a row of buttons
     * drawn under it, one per state. Tapping a button animates to that state: a transformation state plays to its
     * last frame and stays there (switching to another one first plays the current one backwards, back to the start;
     * tapping the active one again goes back to the start); a looping state keeps playing.
     * Without states, an animated GIF just loops.
     */
    const std::vector<AnimationState>& getStates() const;
    /// controlsPart: the part of the element's height taken by the row of buttons
    void setStates(std::vector<AnimationState> states, double controlsPart);
    double getControlsPart() const;
    /// The states in text form, for saving: "name|first|last|loop;..."
    std::string formatStates() const;
    static std::vector<AnimationState> parseStates(const std::string& text);
    /// Reads states from a "xopp-states:" comment in the GIF data (for a GIF being inserted); returns whether found
    bool loadStatesFromGif();
    /// The image size in pixels, plus room for its row of buttons if it has states (for sizing new elements)
    std::pair<int, int> getDisplaySize() const;
    /// Part of the element height the buttons take when inserting a GIF with states
    static constexpr double DEFAULT_CONTROLS_PART = 0.14;

    /// The picture's part of the element, and the rectangle of a button (document coordinates)
    xoj::util::Rectangle<double> getPictureRect() const;
    xoj::util::Rectangle<double> getButtonRect(size_t index) const;
    /// The button at a point (document coordinates), or -1
    int buttonAt(double x, double y) const;
    /// Taps the button of a state (from the main thread)
    void triggerState(size_t index, int64_t now) const;
    /// The state animated to, or -1 for the start
    int getActiveState() const;
    /// Whether frames need advancing now: always for plain animated GIFs, only while changing state for ones with
    /// states
    bool isPlaying() const;

    static constexpr std::pair<int, int> NOSIZE = std::make_pair(-1, -1);

public:
    // Serialize interface
    void serialize(ObjectOutputStream& out) const override;
    void readSerialized(ObjectInputStream& in) override;

private:
    void calcSize() const override;

    /// Renders all frames of an animated GIF, if the data is one and they fit in memory
    void renderAnimation(GdkPixbufAnimation* animation) const;

    /// Frees the frames of an animated GIF
    void clearAnimation() const;
    bool framesReady() const;

private:
    /// Temporary surface used as a render buffer.
    mutable cairo_surface_t* image = nullptr;

    /// Image format information.
    mutable GdkPixbufFormat* format = nullptr;
    mutable std::pair<int, int> imageSize = {-1, -1};

    std::string data;

    /// The frames of an animated GIF (decoded in the background, see renderAnimation())
    mutable std::shared_ptr<AnimationFrames> anim;
    /// The frame drawn now (set by advanceAnimation() in the main thread, read while rendering)
    mutable std::atomic<size_t> currentFrame{0};

    std::vector<AnimationState> states;
    double controlsPart = 0;
    /// Whether the labels are in a right-to-left language (then the first button is on the right)
    bool rightToLeft = false;
    /// Playback of states (main thread only)
    mutable int activeState = -1;   ///< state animated to (-1: the start)
    mutable int pendingState = -1;  ///< state to play once the current one has been played back
    mutable bool playing = false;
    mutable int playDirection = 1;
    mutable size_t stopFrame = 0;
    mutable int64_t nextFrameAt = 0;  ///< µs, g_get_monotonic_time()

    /// Starts playing frames from `from` to `to` (main thread)
    void playSegment(size_t from, size_t to, int64_t now) const;
};
