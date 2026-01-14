package com.termux.x11.input;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.Toast;

import com.termux.x11.Prefs;

import java.util.EnumMap;
import java.util.Locale;
import java.util.Map;

import dev.ilamparithi.lppdebug.helper.LenovoPenButtonListener;
import dev.ilamparithi.lppdebug.helper.PenButtonEvent;

/**
 * Maps Lenovo pen barrel button keycodes (600-604) into stylus button state changes.
 */
public final class LenovoPenButtonMapper {
    private final Context context;
    private final TouchInputHandler inputHandler;
    private final LenovoPenButtonListener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<PenButtonEvent, GestureConfig> configs = new EnumMap<>(PenButtonEvent.class);
    private final Map<PenButtonEvent, Runnable> pendingReleases = new EnumMap<>(PenButtonEvent.class);
    private int toggledMask = 0;
    private boolean showDetections;
    private boolean showToggleDebug;

    public LenovoPenButtonMapper(Context context, TouchInputHandler inputHandler) {
        this.context = context.getApplicationContext();
        this.inputHandler = inputHandler;
        this.listener = new LenovoPenButtonListener(this::handlePenEvent, msg -> {});
    }

    public boolean onKeyEvent(KeyEvent event) {
        return listener.onKeyEvent(event);
    }

    public void reloadPreferences(Prefs prefs) {
        showDetections = prefs.lenovoPenShowDetections.get();
        showToggleDebug = prefs.lenovoPenDebugToggleToasts.get();
        // Clear toggle state and pending releases when preferences change so we don't carry stale bits.
        toggledMask = 0;
        inputHandler.applyStylusToggleMask(0);
        pendingReleases.values().forEach(handler::removeCallbacks);
        pendingReleases.clear();
        configs.put(PenButtonEvent.SINGLE_PRESS, buildConfig(
                prefs.get().getString("lenovoPenSinglePressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenSinglePressToggle", false),
                prefs.get().getString("lenovoPenSinglePressDurationMs", "150")));
        configs.put(PenButtonEvent.DOUBLE_PRESS, buildConfig(
                prefs.get().getString("lenovoPenDoublePressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenDoublePressToggle", false),
                prefs.get().getString("lenovoPenDoublePressDurationMs", "150")));
        configs.put(PenButtonEvent.TRIPLE_PRESS, buildConfig(
                prefs.get().getString("lenovoPenTriplePressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenTriplePressToggle", false),
                prefs.get().getString("lenovoPenTriplePressDurationMs", "150")));
        configs.put(PenButtonEvent.LONG_PRESS, buildConfig(
                prefs.get().getString("lenovoPenLongPressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenLongPressToggle", false),
                prefs.get().getString("lenovoPenLongPressDurationMs", "150")));
        configs.put(PenButtonEvent.LONG_PRESS_AND_CLICK, buildConfig(
                prefs.get().getString("lenovoPenLongPressClickAction", "disabled"),
                prefs.get().getBoolean("lenovoPenLongPressClickToggle", false),
                prefs.get().getString("lenovoPenLongPressClickDurationMs", "150")));
    }

    private GestureConfig buildConfig(String action, boolean toggle, String durationMs) {
        GestureConfig cfg = new GestureConfig();
        cfg.buttonMask = mapActionToButtonMask(action);
        cfg.toggle = toggle;
        try {
            cfg.durationMs = Long.parseLong(durationMs);
        } catch (NumberFormatException e) {
            cfg.durationMs = 150L;
        }
        cfg.durationMs = Math.max(10L, cfg.durationMs);
        return cfg;
    }

    private int mapActionToButtonMask(String action) {
        if (action == null || "disabled".contentEquals(action))
            return 0;
        // Map UI actions to stylus button mask: secondary (right) -> BUTTON_SECONDARY, tertiary (middle) -> BUTTON_TERTIARY.
        if ("2".contentEquals(action))
            return MotionEvent.BUTTON_SECONDARY;
        else if ("3".contentEquals(action))
            return MotionEvent.BUTTON_TERTIARY;
        return 0;
    }

    private void handlePenEvent(PenButtonEvent event) {
        GestureConfig cfg = configs.get(event);
        if (cfg == null)
            return;

        if (showDetections && !(cfg.toggle && showToggleDebug))
            Toast.makeText(context, event.name(), Toast.LENGTH_SHORT).show();

        if (cfg.buttonMask == 0)
            return;

        Runnable pending = pendingReleases.remove(event);
        if (pending != null)
            handler.removeCallbacks(pending);

        if (cfg.toggle) {
            applyToggle(cfg.buttonMask, event);
        } else {
            applyPress(cfg.buttonMask, cfg.durationMs, event);
        }
    }

    private void applyToggle(int buttonMask, PenButtonEvent event) {
        toggledMask ^= buttonMask;
        inputHandler.applyStylusToggleMask(toggledMask);
        if (showToggleDebug) {
            boolean on = (toggledMask & buttonMask) != 0;
            String msg = String.format(Locale.US, "%s toggle %d %s", event.name(), buttonMask, on ? "ON" : "OFF");
            Toast.makeText(context, msg, Toast.LENGTH_SHORT).show();
        }
    }

    private void applyPress(int buttonMask, long durationMs, PenButtonEvent event) {
        StylusState before = inputHandler.getLastStylusState();
        boolean maskWasSet = (before.buttons & buttonMask) != 0;
        inputHandler.sendStylusButtons(before.buttons | buttonMask);

        Runnable release = () -> {
            StylusState current = inputHandler.getLastStylusState();
            int releaseButtons = current.buttons;
            if (maskWasSet)
                releaseButtons |= buttonMask;
            else
                releaseButtons &= ~buttonMask;
            inputHandler.sendStylusButtons(releaseButtons);
        };
        pendingReleases.put(event, release);
        handler.postDelayed(release, Math.max(0, durationMs));
    }

    private static final class GestureConfig {
        int buttonMask;
        boolean toggle;
        long durationMs;
    }
}
