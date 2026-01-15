package com.termux.x11.input;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.Toast;
import android.util.Log;

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
    private int offOnLiftMask = 0;
    private boolean tipDown = false;
    private boolean showDetections;
    private boolean showToggleDebug;

    public LenovoPenButtonMapper(Context context, TouchInputHandler inputHandler) {
        this.context = context.getApplicationContext();
        this.inputHandler = inputHandler;
        this.listener = new LenovoPenButtonListener(this::handlePenEvent, msg -> {});
        this.inputHandler.addStylusStateListener(this::onStylusState);
    }

    public boolean onKeyEvent(KeyEvent event) {
        return listener.onKeyEvent(event);
    }

    public void reloadPreferences(Prefs prefs) {
        showDetections = prefs.lenovoPenShowDetections.get();
        showToggleDebug = prefs.lenovoPenDebugToggleToasts.get();
        // Clear toggle state and pending releases when preferences change so we don't carry stale bits.
        toggledMask = 0;
        offOnLiftMask = 0;
        inputHandler.applyStylusToggleMask(0);
        pendingReleases.values().forEach(handler::removeCallbacks);
        pendingReleases.clear();
        configs.put(PenButtonEvent.SINGLE_PRESS, buildConfig(
                prefs.get().getString("lenovoPenSinglePressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenSinglePressToggle", false),
            prefs.get().getBoolean("lenovoPenSinglePressToggleOffOnLift", false),
                prefs.get().getString("lenovoPenSinglePressDurationMs", "150")));
        configs.put(PenButtonEvent.DOUBLE_PRESS, buildConfig(
                prefs.get().getString("lenovoPenDoublePressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenDoublePressToggle", false),
            prefs.get().getBoolean("lenovoPenDoublePressToggleOffOnLift", false),
                prefs.get().getString("lenovoPenDoublePressDurationMs", "150")));
        configs.put(PenButtonEvent.TRIPLE_PRESS, buildConfig(
                prefs.get().getString("lenovoPenTriplePressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenTriplePressToggle", false),
            prefs.get().getBoolean("lenovoPenTriplePressToggleOffOnLift", false),
                prefs.get().getString("lenovoPenTriplePressDurationMs", "150")));
        configs.put(PenButtonEvent.LONG_PRESS, buildConfig(
                prefs.get().getString("lenovoPenLongPressAction", "disabled"),
                prefs.get().getBoolean("lenovoPenLongPressToggle", false),
            prefs.get().getBoolean("lenovoPenLongPressToggleOffOnLift", false),
                prefs.get().getString("lenovoPenLongPressDurationMs", "150")));
        configs.put(PenButtonEvent.LONG_PRESS_AND_CLICK, buildConfig(
                prefs.get().getString("lenovoPenLongPressClickAction", "disabled"),
                prefs.get().getBoolean("lenovoPenLongPressClickToggle", false),
            prefs.get().getBoolean("lenovoPenLongPressClickToggleOffOnLift", false),
                prefs.get().getString("lenovoPenLongPressClickDurationMs", "150")));

        validateConfigs();
    }

    private GestureConfig buildConfig(String action, boolean toggle, boolean toggleOffOnLift, String durationMs) {
        GestureConfig cfg = new GestureConfig();
        cfg.buttonMask = mapActionToButtonMask(action);
        cfg.toggle = toggle;
        cfg.toggleOffOnLift = toggleOffOnLift;
        try {
            cfg.durationMs = Long.parseLong(durationMs);
        } catch (NumberFormatException e) {
            cfg.durationMs = 150L;
        }
        cfg.durationMs = Math.max(10L, Math.min(8192L, cfg.durationMs));
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

        // Latest gesture wins: cancel pending releases touching the same buttons.
        cancelPendingForButtons(cfg.buttonMask);

        if (cfg.toggle) {
            applyToggle(cfg.buttonMask, event);
        } else {
            applyPress(cfg.buttonMask, cfg.durationMs, event);
        }
    }

    private void cancelPendingForButtons(int buttonMask) {
        for (Map.Entry<PenButtonEvent, GestureConfig> entry : configs.entrySet()) {
            Runnable pending = pendingReleases.get(entry.getKey());
            if (pending == null)
                continue;
            GestureConfig cfg = entry.getValue();
            if (cfg != null && (cfg.buttonMask & buttonMask) != 0) {
                handler.removeCallbacks(pending);
                pendingReleases.remove(entry.getKey());
            }
        }
    }

    private void applyToggle(int buttonMask, PenButtonEvent event) {
        // Toggle overrides any active press on the same buttons.
        toggledMask &= ~buttonMask;
        toggledMask ^= buttonMask;
        boolean on = (toggledMask & buttonMask) != 0;
        StylusState current = inputHandler.getLastStylusState();
        int targetButtons = (current.buttons & ~buttonMask) | (on ? buttonMask : 0);
        inputHandler.sendStylusButtons(targetButtons);
        inputHandler.applyStylusToggleMask(toggledMask);

        GestureConfig cfg = configs.get(event);
        if (cfg != null && cfg.toggleOffOnLift) {
            if (on)
                offOnLiftMask |= buttonMask;
            else
                offOnLiftMask &= ~buttonMask;
        }

        if (showToggleDebug) {
            String msg = String.format(Locale.US, "%s toggle %d %s", event.name(), buttonMask, on ? "ON" : "OFF");
            Toast.makeText(context, msg, Toast.LENGTH_SHORT).show();
        }
    }

    private void applyPress(int buttonMask, long durationMs, PenButtonEvent event) {
        // Press overrides toggle state for these buttons.
        if ((toggledMask & buttonMask) != 0) {
            toggledMask &= ~buttonMask;
            inputHandler.applyStylusToggleMask(toggledMask);
        }
        offOnLiftMask &= ~buttonMask;
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
        boolean toggleOffOnLift;
        long durationMs;
    }

    private void onStylusState(StylusState state) {
        boolean nowTipDown = (state.buttons & MotionEvent.BUTTON_PRIMARY) != 0 || state.pressure > 0;
        boolean wasTipDown = tipDown;
        tipDown = nowTipDown;

        if (!wasTipDown && nowTipDown) {
            // Arm cycle; nothing else needed.
            return;
        }

        if (wasTipDown && !nowTipDown && offOnLiftMask != 0) {
            int mask = offOnLiftMask;
            toggledMask &= ~mask;
            offOnLiftMask &= ~mask;
            inputHandler.applyStylusToggleMask(toggledMask);
            StylusState current = inputHandler.getLastStylusState();
            int targetButtons = current.buttons & ~mask;
            inputHandler.sendStylusButtons(targetButtons);
        }
    }

    private void validateConfigs() {
        // Disable conflicting mappings: same button mask with the same mode.
        PenButtonEvent[] events = PenButtonEvent.values();
        for (int i = 0; i < events.length; i++) {
            GestureConfig a = configs.get(events[i]);
            if (a == null || a.buttonMask == 0)
                continue;
            for (int j = i + 1; j < events.length; j++) {
                GestureConfig b = configs.get(events[j]);
                if (b == null || b.buttonMask == 0)
                    continue;
                if (a.buttonMask == b.buttonMask && a.toggle == b.toggle) {
                    Log.w("LenovoPenButtonMapper", "Conflicting mapping for " + events[i] + " and " + events[j] + " with mask " + a.buttonMask + " and mode " + (a.toggle ? "toggle" : "press") + "; disabling " + events[j]);
                    b.buttonMask = 0;
                }
            }
        }
    }
}
