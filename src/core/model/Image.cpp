#include "Image.h"

#include <algorithm>  // for min
#include <array>      // for array
#include <cmath>      // for sqrt
#include <cstring>    // for strcmp
#include <memory>
#include <string_view>  // for string_view
#include <utility>  // for move, pair

#include <cairo.h>    // for cairo_surface_destroy
#include <gdk/gdk.h>  // for gdk_cairo_set_sourc...
#include <glib.h>     // for guchar

#include "model/Element.h"   // for Element, ELEMENT_IMAGE
#include "util/Assert.h"     // for xoj_assert
#include "util/Rectangle.h"  // for Rectangle
#include "util/i18n.h"
#include "util/raii/GObjectSPtr.h"  // for GObjectSPtr
#include "util/safe_casts.h"
#include "util/serializing/ObjectInputStream.h"   // for ObjectInputStream
#include "util/serializing/ObjectOutputStream.h"  // for ObjectOutputStream

using xoj::util::Rectangle;

Image::Image(): Element(ELEMENT_IMAGE) {}

namespace {
/// Largest memory (bytes) the rendered frames of one animated GIF may take. Bigger GIFs show their first frame.
constexpr uint64_t MAX_ANIMATION_BYTES = 256u << 20;
/// Shortest time a frame is shown (ms), as browsers do: GIFs with 0 delays would otherwise spin as fast as possible
constexpr int MIN_FRAME_DELAY = 20;

/// Number of frames in GIF data, found by walking its blocks (without decoding the pixels). 0 if it isn't a GIF.
auto countGifFrames(std::string_view d) -> size_t {
    if (d.size() < 13 || d.substr(0, 4) != "GIF8") {
        return 0;
    }
    auto byte = [&](size_t i) { return static_cast<unsigned char>(d[i]); };
    size_t pos = 13;
    if (byte(10) & 0x80) {  // global color table
        pos += 3u * (1u << ((byte(10) & 7) + 1));
    }
    auto skipSubBlocks = [&]() {
        while (pos < d.size()) {
            size_t len = byte(pos++);
            if (len == 0) {
                break;
            }
            pos += len;
        }
    };
    size_t count = 0;
    while (pos < d.size()) {
        unsigned char block = byte(pos++);
        if (block == 0x2C) {  // image descriptor
            if (pos + 9 > d.size()) {
                break;
            }
            unsigned char flags = byte(pos + 8);
            pos += 9;
            if (flags & 0x80) {  // local color table
                pos += 3u * (1u << ((flags & 7) + 1));
            }
            pos++;  // LZW minimum code size
            skipSubBlocks();
            count++;
        } else if (block == 0x21) {  // extension
            pos++;                   // label
            skipSubBlocks();
        } else {  // 0x3B trailer, or broken data
            break;
        }
    }
    return count;
}

/// Copies a pixbuf to a new cairo image surface
auto surfaceFromPixbuf(GdkPixbuf* pixbuf) -> cairo_surface_t* {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gdk_pixbuf_get_width(pixbuf),
                                                          gdk_pixbuf_get_height(pixbuf));
    cairo_t* cr = cairo_create(surface);
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    return surface;
}
}  // namespace

Image::~Image() {
    clearAnimation();
    if (this->image) {
        cairo_surface_destroy(this->image);
        this->image = nullptr;
    }

    if (this->format) {
        gdk_pixbuf_format_free(this->format);
        this->format = nullptr;
    }
}

auto Image::clone() const -> ElementPtr {
    auto img = std::make_unique<Image>();

    img->x = this->x;
    img->y = this->y;
    img->setColor(this->getColor());
    img->width = this->width;
    img->height = this->height;
    img->data = this->data;
    img->states = this->states;
    img->controlsPart = this->controlsPart;

    img->image = cairo_surface_reference(this->image);
    if (this->format) {
        img->format = gdk_pixbuf_format_copy(this->format);
    }
    img->imageSize = this->imageSize;
    if (this->framesReady) {
        for (cairo_surface_t* frame: this->frames) {
            img->frames.push_back(cairo_surface_reference(frame));
        }
        img->frameDelays = this->frameDelays;
        img->animationDuration = this->animationDuration;
        img->currentFrame = this->currentFrame.load();
        img->framesReady = true;
    }
    img->snappedBounds = this->snappedBounds;
    img->sizeCalculated = this->sizeCalculated;

    return img;
}

void Image::setWidth(double width) {
    this->width = width;
    this->calcSize();
}

void Image::setHeight(double height) {
    this->height = height;
    this->calcSize();
}

void Image::setImage(std::string_view data) { setImage(std::string(data)); }

void Image::setImage(std::string&& data) {
    clearAnimation();
    if (this->image) {
        cairo_surface_destroy(this->image);
        this->image = nullptr;
    }
    this->data = std::move(data);

    if (this->format) {
        gdk_pixbuf_format_free(this->format);
        this->format = nullptr;
    }

    // FIXME: awful hack to try to parse the format
    std::array<char*, 4096> buffer{};
    xoj::util::GObjectSPtr<GdkPixbufLoader> loader(gdk_pixbuf_loader_new(), xoj::util::adopt);
    size_t remaining = this->data.size();
    while (remaining > 0) {
        size_t readLen = std::min(remaining, buffer.size());
        if (!gdk_pixbuf_loader_write(loader.get(), reinterpret_cast<const guchar*>(this->data.c_str()), readLen,
                                     nullptr))
            break;
        remaining -= readLen;

        // Try to determine the format early, if possible
        this->format = gdk_pixbuf_loader_get_format(loader.get());
        if (this->format) {
            break;
        }
    }
    gdk_pixbuf_loader_close(loader.get(), nullptr);
    // if the format was not determined early, it can probably be determined now
    if (!this->format) {
        this->format = gdk_pixbuf_loader_get_format(loader.get());
    }
    xoj_assert_message(this->format != nullptr, "could not parse the image format!");

    // the format is owned by the pixbuf, so create a copy
    this->format = gdk_pixbuf_format_copy(this->format);
}

void Image::setImage(GdkPixbuf* img) {
    if (this->image) {
        cairo_surface_destroy(this->image);
        this->image = nullptr;
    }
    this->imageSize = {gdk_pixbuf_get_width(img), gdk_pixbuf_get_height(img)};

    this->image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, this->imageSize.first, this->imageSize.second);
    xoj_assert(this->image != nullptr);

    // Paint the pixbuf on to the surface
    cairo_t* cr = cairo_create(this->image);
    gdk_cairo_set_source_pixbuf(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    const cairo_write_func_t writeFunc = [](void* bufferPtr, const unsigned char* data,
                                            unsigned int length) -> cairo_status_t {
        reinterpret_cast<decltype(Image::data)*>(bufferPtr)->append(reinterpret_cast<const char*>(data), length);
        return CAIRO_STATUS_SUCCESS;
    };
    cairo_surface_write_to_png_stream(image, writeFunc, &data);
}

auto Image::renderBuffer() const -> std::optional<std::string> {
    xoj_assert_message(data.length() > 0, "image has no data, cannot render it!");
    if (this->image) {
        // Already rendered
        return std::nullopt;
    }
    xoj::util::GObjectSPtr<GdkPixbufLoader> loader(gdk_pixbuf_loader_new(), xoj::util::adopt);
    g_signal_connect(loader.get(), "size-prepared",
                     G_CALLBACK(+[](GdkPixbufLoader* self, gint width, gint height, gpointer) {
                         static constexpr uint64_t MAX_SIZE =
                                 1 << 25;  ///< Max number of pixels: 32M = more than enough for A4 in 72pp
                         if (width <= 0 || height <= 0) {
                             g_warning("Image::renderBuffer(): non-positive width/height");
                             return;
                         }
                         if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > MAX_SIZE) {
                             double ratio = static_cast<double>(width) / static_cast<double>(height);
                             gint maxHeight = floor_cast<gint>(std::sqrt(MAX_SIZE / ratio));
                             gint maxWidth = floor_cast<gint>(maxHeight * ratio);
                             g_warning("Trying to open an image too big %d x %d. Resizing it to %d x %d", width, height,
                                       maxWidth, maxHeight);
                             gdk_pixbuf_loader_set_size(self, maxHeight, maxWidth);
                         }
                     }),
                     nullptr);
    GError* err = nullptr;
    bool success = gdk_pixbuf_loader_write(loader.get(), reinterpret_cast<const guchar*>(this->data.c_str()),
                                           this->data.length(), &err);
    if (!success) {
        if (err != nullptr) {
            std::string msg = std::string(_("Failed to load image")) + "\n" + _("Error: ") + err->message;
            g_free(err);
            return msg;
        } else {
            return std::string(_("Failed to load image")) + "\n" + _("Unrecoverable error");
        }
    }
    success = gdk_pixbuf_loader_close(loader.get(), &err);
    if (!success) {
        if (err != nullptr) {
            std::string msg = std::string(_("Failed to close image stream")) + "\n" + _("Error: ") + err->message;
            g_free(err);
            return msg;
        } else {
            return std::string(_("Failed to close image stream")) + "\n" + _("Unrecoverable error");
        }
    }

    GdkPixbuf* tmp = gdk_pixbuf_loader_get_pixbuf(loader.get());
    xoj_assert(tmp != nullptr);
    xoj::util::GObjectSPtr<GdkPixbuf> pixbuf(gdk_pixbuf_apply_embedded_orientation(tmp), xoj::util::adopt);

    this->imageSize = {gdk_pixbuf_get_width(pixbuf.get()), gdk_pixbuf_get_height(pixbuf.get())};

    // TODO: pass in window once this code is refactored into ImageView
    this->image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, this->imageSize.first, this->imageSize.second);
    g_assert(this->image != nullptr);

    // Paint the pixbuf on to the surface
    // NOTE: we do this manually instead of using gdk_cairo_surface_create_from_pixbuf
    // since this does not work in CLI mode.
    cairo_t* cr = cairo_create(this->image);
    gdk_cairo_set_source_pixbuf(cr, pixbuf.get(), 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    if (GdkPixbufAnimation* animation = gdk_pixbuf_loader_get_animation(loader.get());
        animation && !gdk_pixbuf_animation_is_static_image(animation)) {
        renderAnimation(animation);
    }
    return std::nullopt;
}

void Image::renderAnimation(GdkPixbufAnimation* animation) const {
    const size_t count = countGifFrames(this->data);
    const int w = gdk_pixbuf_animation_get_width(animation);
    const int h = gdk_pixbuf_animation_get_height(animation);
    if (count < 2 || w <= 0 || h <= 0) {
        return;
    }
    if (static_cast<uint64_t>(count) * static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 > MAX_ANIMATION_BYTES) {
        g_warning("Animated GIF too big to animate (%zu frames of %d x %d): showing its first frame", count, w, h);
        return;
    }
    // Step through the frames with gdk-pixbuf's iterator: it composes each frame onto the previous ones as the
    // GIF says (transparency, disposal), so every frame is a complete picture
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS  // gdk-pixbuf's animation API still takes a GTimeVal
            GTimeVal time = {0, 0};
    xoj::util::GObjectSPtr<GdkPixbufAnimationIter> iter(gdk_pixbuf_animation_get_iter(animation, &time),
                                                        xoj::util::adopt);
    std::vector<cairo_surface_t*> newFrames;
    std::vector<int> delays;
    int64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        GdkPixbuf* pixbuf = gdk_pixbuf_animation_iter_get_pixbuf(iter.get());
        if (!pixbuf || gdk_pixbuf_get_width(pixbuf) != w || gdk_pixbuf_get_height(pixbuf) != h) {
            break;
        }
        newFrames.push_back(surfaceFromPixbuf(pixbuf));
        int delay = gdk_pixbuf_animation_iter_get_delay_time(iter.get());
        if (delay < 0) {  // the last frame stays forever
            delays.push_back(-1);
            break;
        }
        delay = std::max(delay, MIN_FRAME_DELAY);
        delays.push_back(delay);
        total += delay;
        g_time_val_add(&time, static_cast<glong>(delay) * 1000);
        gdk_pixbuf_animation_iter_advance(iter.get(), &time);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (newFrames.size() < 2 || total <= 0 || delays.back() < 0) {
        // nothing to animate (or an animation that stops: shown as its first frame)
        for (cairo_surface_t* f: newFrames) {
            cairo_surface_destroy(f);
        }
        return;
    }
    this->frames = std::move(newFrames);
    this->frameDelays = std::move(delays);
    this->animationDuration = total;
    this->currentFrame = 0;
    this->framesReady = true;
}

void Image::clearAnimation() const {
    this->framesReady = false;
    for (cairo_surface_t* frame: this->frames) {
        cairo_surface_destroy(frame);
    }
    this->frames.clear();
    this->frameDelays.clear();
    this->animationDuration = 0;
    this->currentFrame = 0;
}

auto Image::isGif() const -> bool {
    return this->format && std::strcmp(gdk_pixbuf_format_get_name(this->format), "gif") == 0;
}

auto Image::isAnimated() const -> bool { return this->framesReady; }

auto Image::advanceAnimation(int64_t now) const -> bool {
    if (!this->framesReady) {
        return false;
    }
    if (!this->states.empty()) {
        if (!this->playing || now < this->nextFrameAt) {
            return false;
        }
        size_t frame = this->currentFrame;
        if (frame == this->stopFrame) {
            // this part is done
            if (this->pendingState >= 0) {
                const AnimationState& s = this->states[static_cast<size_t>(this->pendingState)];
                this->pendingState = -1;
                playSegment(s.first, s.last, now);
                return false;
            }
            if (this->activeState >= 0 && this->states[static_cast<size_t>(this->activeState)].loop) {
                const AnimationState& s = this->states[static_cast<size_t>(this->activeState)];
                this->currentFrame = std::min(s.first, this->frames.size() - 1);
                this->nextFrameAt = now + static_cast<int64_t>(this->frameDelays[this->currentFrame]) * 1000;
                return true;
            }
            this->playing = false;
            return false;
        }
        frame = this->playDirection > 0 ? frame + 1 : frame - 1;
        this->currentFrame = frame;
        this->nextFrameAt = now + static_cast<int64_t>(this->frameDelays[frame]) * 1000;
        return true;
    }
    int64_t t = (now / 1000) % this->animationDuration;
    size_t frame = 0;
    while (frame + 1 < this->frameDelays.size() && t >= this->frameDelays[frame]) {
        t -= this->frameDelays[frame];
        frame++;
    }
    if (frame == this->currentFrame) {
        return false;
    }
    this->currentFrame = frame;
    return true;
}

auto Image::msUntilNextFrame(int64_t now) const -> int {
    if (!this->framesReady) {
        return 1000;
    }
    if (!this->states.empty()) {
        return static_cast<int>(std::clamp<int64_t>((this->nextFrameAt - now + 999) / 1000, 1, 1000));
    }
    int64_t t = (now / 1000) % this->animationDuration;
    for (int delay: this->frameDelays) {
        if (t < delay) {
            return std::max(static_cast<int>(delay - t), 1);
        }
        t -= delay;
    }
    return 1;
}

auto Image::getImage() const -> cairo_surface_t* {
    if (auto opt = renderBuffer(); opt.has_value()) {
        // An error occurred
        g_warning("%s", opt->c_str());
    }
    if (this->framesReady) {
        return this->frames[std::min(this->currentFrame.load(), this->frames.size() - 1)];
    }
    return this->image;
}

void Image::scale(double x0, double y0, double fx, double fy, double rotation,
                  bool) {  // line width scaling option is not used
    this->x -= x0;
    this->x *= fx;
    this->x += x0;
    this->y -= y0;
    this->y *= fy;
    this->y += y0;

    this->width *= fx;
    this->height *= fy;
    this->calcSize();
}

void Image::rotate(double x0, double y0, double th) {}

auto Image::getStates() const -> const std::vector<AnimationState>& { return this->states; }

void Image::setStates(std::vector<AnimationState> states, double controlsPart) {
    this->states = std::move(states);
    this->controlsPart = this->states.empty() ? 0 : std::clamp(controlsPart, 0.0, 0.5);
    this->activeState = -1;
    this->pendingState = -1;
    this->playing = false;
}

auto Image::getControlsPart() const -> double { return this->controlsPart; }

auto Image::getDisplaySize() const -> std::pair<int, int> {
    auto [w, h] = getImageSize();
    if (h > 0 && this->controlsPart > 0) {
        h = static_cast<int>(std::lround(h / (1 - this->controlsPart)));
    }
    return {w, h};
}

auto Image::formatStates() const -> std::string {
    std::string s;
    for (const AnimationState& st: this->states) {
        if (!s.empty()) {
            s += ';';
        }
        std::string name = st.name;
        for (char& c: name) {
            if (c == '|' || c == ';') {
                c = ' ';
            }
        }
        s += name + '|' + std::to_string(st.first) + '|' + std::to_string(st.last) + '|' + (st.loop ? "loop" : "once");
    }
    return s;
}

auto Image::parseStates(const std::string& text) -> std::vector<AnimationState> {
    std::vector<AnimationState> states;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string entry = text.substr(pos, end - pos);
        pos = end + 1;
        std::vector<std::string> f;
        size_t p = 0;
        while (true) {
            size_t q = entry.find('|', p);
            f.push_back(entry.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) {
                break;
            }
            p = q + 1;
        }
        if (f.size() < 3 || f[0].empty()) {
            continue;
        }
        char* endPtr = nullptr;
        const unsigned long first = std::strtoul(f[1].c_str(), &endPtr, 10);
        const unsigned long last = std::strtoul(f[2].c_str(), &endPtr, 10);
        if (states.size() >= 32 || first > 100000 || last > 100000) {
            continue;
        }
        states.push_back({f[0], first, last, f.size() > 3 && f[3] == "loop"});
    }
    return states;
}

auto Image::loadStatesFromGif() -> bool {
    // GIF comment extensions: 0x21 0xFE, then sub-blocks
    std::string_view d = this->data;
    if (d.size() < 13 || d.substr(0, 4) != "GIF8") {
        return false;
    }
    const std::string_view key = "xopp-states:";
    for (size_t pos = d.find("\x21\xFE"); pos != std::string_view::npos; pos = d.find("\x21\xFE", pos + 2)) {
        std::string text;
        size_t i = pos + 2;
        while (i < d.size()) {
            const size_t len = static_cast<unsigned char>(d[i++]);
            if (len == 0 || i + len > d.size()) {
                break;
            }
            text.append(d.substr(i, len));
            i += len;
        }
        if (text.compare(0, key.size(), key) == 0) {
            auto parsed = parseStates(text.substr(key.size()));
            if (!parsed.empty()) {
                setStates(std::move(parsed), DEFAULT_CONTROLS_PART);
                return true;
            }
        }
    }
    return false;
}

auto Image::getPictureRect() const -> xoj::util::Rectangle<double> {
    return {this->x, this->y, this->width, this->height * (1 - this->controlsPart)};
}

auto Image::getButtonRect(size_t index) const -> xoj::util::Rectangle<double> {
    const double rowY = this->y + this->height * (1 - this->controlsPart);
    const double rowH = this->height * this->controlsPart;
    const double n = static_cast<double>(std::max<size_t>(this->states.size(), 1));
    const double gap = std::min(rowH * 0.2, this->width * 0.02);
    const double w = (this->width - gap * (n + 1)) / n;
    return {this->x + gap + static_cast<double>(index) * (w + gap), rowY + gap, w, rowH - 2 * gap};
}

auto Image::buttonAt(double px, double py) const -> int {
    for (size_t i = 0; i < this->states.size(); i++) {
        auto r = getButtonRect(i);
        if (px >= r.x && px <= r.x + r.width && py >= r.y && py <= r.y + r.height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Image::playSegment(size_t from, size_t to, int64_t now) const {
    const size_t lastFrame = this->frames.size() - 1;
    from = std::min(from, lastFrame);
    to = std::min(to, lastFrame);
    this->currentFrame = from;
    this->stopFrame = to;
    this->playDirection = to >= from ? 1 : -1;
    this->playing = true;
    this->nextFrameAt = now + static_cast<int64_t>(this->frameDelays[from]) * 1000;
}

void Image::triggerState(size_t index, int64_t now) const {
    if (index >= this->states.size() || !this->framesReady) {
        return;
    }
    const AnimationState& target = this->states[index];
    const int current = this->activeState;
    if (current == static_cast<int>(index)) {
        // tapping the active state again: back to the start
        this->activeState = -1;
        this->pendingState = -1;
        if (target.loop) {
            playSegment(target.first, target.first, now);
        } else {
            playSegment(this->currentFrame, target.first, now);
        }
        return;
    }
    this->activeState = static_cast<int>(index);
    if (current < 0 && this->playing && this->playDirection < 0) {
        // still going back to the start: finish that, then play the new state
        this->pendingState = static_cast<int>(index);
        return;
    }
    if (current >= 0 && !this->states[static_cast<size_t>(current)].loop) {
        // first undo the current transformation, then play the new one
        playSegment(this->currentFrame, this->states[static_cast<size_t>(current)].first, now);
        this->pendingState = static_cast<int>(index);
    } else {
        this->pendingState = -1;
        playSegment(target.first, target.last, now);
    }
}

auto Image::getActiveState() const -> int { return this->activeState; }

auto Image::isPlaying() const -> bool { return this->framesReady && (this->states.empty() || this->playing); }

void Image::serialize(ObjectOutputStream& out) const {
    out.writeObject("Image");

    this->Element::serialize(out);

    out.writeDouble(this->width);
    out.writeDouble(this->height);

    out.writeImage(this->data);
    out.writeString(formatStates());
    out.writeDouble(this->controlsPart);

    out.endObject();
}

void Image::readSerialized(ObjectInputStream& in) {
    in.readObject("Image");

    this->Element::readSerialized(in);

    this->width = in.readDouble();
    this->height = in.readDouble();

    // (setImage also finds the format: without it, a pasted GIF wouldn't be saved as a GIF)
    setImage(in.readImage());
    std::string statesText = in.readString();
    const double part = in.readDouble();
    setStates(parseStates(statesText), part);

    in.endObject();
    this->calcSize();
}

void Image::calcSize() const {
    this->snappedBounds = Rectangle<double>(this->x, this->y, this->width, this->height);
    this->sizeCalculated = true;
}

bool Image::hasData() const { return !this->data.empty(); }

const unsigned char* Image::getRawData() const { return reinterpret_cast<const unsigned char*>(this->data.data()); }

size_t Image::getRawDataLength() const { return this->data.size(); }

std::pair<int, int> Image::getImageSize() const { return this->imageSize; }

GdkPixbufFormat* Image::getImageFormat() const { return this->format; }
