/*
 * Xournal++
 *
 * [Header description]
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include "PenInputHandler.h"  // for PenInputHandler

class InputContext;
struct InputEvent;

class StylusInputHandler: public PenInputHandler {
public:
    explicit StylusInputHandler(InputContext* inputContext);
    ~StylusInputHandler() override;

    bool handleImpl(InputEvent const& event) override;

    /**
     * @brief Change the tool based on the settings and the Button pressed.
     * In case no button is pressed or the settings for that button are "No Toolchange" this function ensures that the
     * tool selected in the toolbar remains selected.
     *
     * @param event
     * @return true if tool was changed successfully
     * @return false if tool was not changed successfully
     */
    bool changeTool(InputEvent const& event) override;

private:
    /**
     * How many events since hitting the screen with the stylus are still left to be ignored before actually starting
     * the action. Set according to the user's setting on every button-1-press-event. Will be evaluated and then
     * decreased by one (down to -1) on every button-1-press-event and motion-event. Set to -1 on button-1-release.\n
     * If >0: Ignore the (button-1-press- or motion-) event\n
     * If 0: Start the action (on the next button-1-press- or motion-event)\n
     * If -1: Stylus not touching or action already started, handle the event normally
     */
    int eventsToIgnore = -1;

    void setPressedState(InputEvent const& event);

    bool handleEvent(InputEvent const& event);

    /**
     * Tap or hold: a stylus button set to open the floating toolbox on a quick tap still works as configured while it
     * is held (e.g. as the hand tool for scrolling). Returns true on the release of such a quick tap.
     */
    bool isButtonTap(InputEvent const& event);

    struct ButtonTap {
        guint button = 0;  ///< 0 when no tap is under way
        guint32 time = 0;
        double x = 0, y = 0;
    } buttonTap;

    /// A press shorter than this (ms) without moving further than this (screen pixels) is a tap
    static constexpr guint32 BUTTON_TAP_MAX_TIME = 350;
    static constexpr double BUTTON_TAP_MAX_DISTANCE = 10;
};
