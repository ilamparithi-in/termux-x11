package com.termux.x11.input;

public final class StylusState {
    public float x;
    public float y;
    public int pressure;
    public int tiltX;
    public int tiltY;
    public int orientation;
    public int buttons;
    public boolean eraser;
    public boolean mouse;

    public StylusState() {}

    public StylusState copy() {
        StylusState copy = new StylusState();
        copy.setFrom(this);
        return copy;
    }

    public StylusState withButtons(int newButtons) {
        StylusState copy = copy();
        copy.buttons = newButtons;
        return copy;
    }

    public void setFrom(StylusState other) {
        x = other.x;
        y = other.y;
        pressure = other.pressure;
        tiltX = other.tiltX;
        tiltY = other.tiltY;
        orientation = other.orientation;
        buttons = other.buttons;
        eraser = other.eraser;
        mouse = other.mouse;
    }
}
