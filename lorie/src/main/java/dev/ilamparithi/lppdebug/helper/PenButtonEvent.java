package dev.ilamparithi.lppdebug.helper;

import java.util.Arrays;
import java.util.Optional;

public enum PenButtonEvent {
    SINGLE_PRESS(600),
    DOUBLE_PRESS(601),
    TRIPLE_PRESS(602),
    LONG_PRESS(603),
    LONG_PRESS_AND_CLICK(604);

    private final int keyCode;

    PenButtonEvent(int keyCode) {
        this.keyCode = keyCode;
    }

    public int getKeyCode() {
        return keyCode;
    }

    public static Optional<PenButtonEvent> fromKeyCode(int keyCode) {
        return Arrays.stream(values()).filter(v -> v.keyCode == keyCode).findFirst();
    }
}
