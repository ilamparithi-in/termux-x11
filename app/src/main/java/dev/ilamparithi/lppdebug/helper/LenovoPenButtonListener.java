package dev.ilamparithi.lppdebug.helper;

import android.view.KeyEvent;

import java.util.function.Consumer;

/**
 * Portable listener for Lenovo Pen Plus button keycodes (600-604).
 * Emits a single event per gesture and consumes the KeyEvent when handled.
 */
public class LenovoPenButtonListener {
    private final Consumer<PenButtonEvent> onButtonPressed;
    private final Consumer<String> onDebug;
    private Integer pendingKeyCode = null;
    private long lastEmittedAt = 0L;

    public LenovoPenButtonListener(Consumer<PenButtonEvent> onButtonPressed, Consumer<String> onDebug) {
        this.onButtonPressed = onButtonPressed;
        this.onDebug = onDebug;
    }

    /**
     * Drop-in handler for dispatchKeyEvent() or onKeyUp().
     * Consumes stylus keycodes 600-604 and emits exactly once per press.
     */
    public boolean onKeyEvent(KeyEvent event) {
        if (event == null || !isLenovoPenButton(event.getKeyCode()))
            return false;

        if (event.getRepeatCount() > 0)
            return true; // ignore repeats to avoid noise

        switch (event.getAction()) {
            case KeyEvent.ACTION_DOWN:
                pendingKeyCode = event.getKeyCode();
                debug("Pen button DOWN code=" + event.getKeyCode());
                return true;
            case KeyEvent.ACTION_UP:
                int codeToEmit = pendingKeyCode != null ? pendingKeyCode : event.getKeyCode();
                pendingKeyCode = null;
                PenButtonEvent.fromKeyCode(codeToEmit).ifPresent(buttonEvent -> {
                    if (event.getEventTime() != lastEmittedAt) {
                        lastEmittedAt = event.getEventTime();
                        onButtonPressed.accept(buttonEvent);
                        debug("Pen button UP code=" + codeToEmit + " -> " + buttonEvent);
                    }
                });
                return true;
            default:
                return false;
        }
    }

    /** Convenience wrapper matching Activity.onKeyUp signature. */
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return onKeyEvent(event);
    }

    private boolean isLenovoPenButton(int keyCode) {
        return keyCode >= 600 && keyCode <= 604;
    }

    private void debug(String msg) {
        if (onDebug != null)
            onDebug.accept(msg);
    }
}
