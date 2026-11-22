package com.tashkeel;

import java.io.Closeable;

public final class TashkeelEngine implements Closeable {
    static {
        System.loadLibrary("tashkeel");
    }

    private long handle;

    public TashkeelEngine(String modelPath) {
        this.handle = nativeCreate(modelPath);
        if (this.handle == 0) {
            throw new IllegalStateException("Failed to load .tash model: " + modelPath);
        }
    }

    public String diacritize(String text) {
        if (handle == 0 || text == null || text.isEmpty()) return text;
        return nativeDiacritize(handle, text);
    }

    public int getVocabSize() {
        return handle == 0 ? 0 : nativeVocabSize(handle);
    }

    public boolean setWeight(int id, int value) {
        return handle != 0 && nativeSetWeight(handle, id, value);
    }

    public void setRestoreSpelling(boolean enabled) {
        if (handle != 0) nativeSetRestoreSpelling(handle, enabled);
    }

    @Override
    public void close() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private static native long nativeCreate(String modelPath);
    private static native void nativeDestroy(long handle);
    private static native String nativeDiacritize(long handle, String text);
    private static native int nativeVocabSize(long handle);
    private static native boolean nativeSetWeight(long handle, int id, int value);
    private static native void nativeSetRestoreSpelling(long handle, boolean enabled);
}
