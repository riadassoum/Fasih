package com.planetblindtech.barq;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;

import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.locks.ReentrantReadWriteLock;

/**
 * BARQ Android Engine Wrapper — High-Speed Arabic Diacritization (Neural I'rab + Statistical Core).
 *
 * <p>Developed by <b>Planet BlindTech (كوكب المكفوفين التقني)</b>.</p>
 *
 * <p>Thread-safe {@link AutoCloseable} wrapper around native {@code libbarq.so}. Multiple threads
 * may call {@link #diacritize(String)}, {@link #diacritizeBatch(String[])}, and
 * {@link #diacritizeLines(List)} concurrently without external synchronization.</p>
 *
 * <pre>{@code
 * try (barq engine = barq.fromAssets(context)) {
 *     String vocalized = engine.diacritize("ذهب الطالب إلى المدرسة صباحا");
 * }
 * }</pre>
 */
public final class barq implements Closeable, AutoCloseable {

    static {
        System.loadLibrary("barq");
    }

    // Core statistical weights (format.hpp WeightId)
    public static final int W_LEX = 0;
    public static final int W_BIGRAM = 1;
    public static final int W_TRANS = 2;
    public static final int W_OOV_TREE = 3;
    public static final int W_IRAB_TREE = 4;
    public static final int W_RULE = 5;
    public static final int W_UNARY_RULE = 6;
    public static final int W_PROCLITIC_PEN = 7;
    public static final int W_SUFFIX_PEN = 8;
    public static final int W_RELAXED_PEN = 9;
    public static final int W_EXPAND_PEN = 10;
    public static final int W_CONSTRAINT = 11;
    public static final int W_MORPH_TREE = 12;

    // V7 syntactic/contextual weights (format.hpp V7WeightId, offset +100)
    public static final int V7W_NGRAM_LEFT = 100;
    public static final int V7W_RBG = 101;
    public static final int V7W_CTR3 = 102;
    public static final int V7W_SKIP1 = 103;
    public static final int V7W_CORE_BG_L = 104;
    public static final int V7W_CORE_BG_R = 105;
    public static final int V7W_CORE_BG_C = 106;
    public static final int V7W_TRANS3 = 107;
    public static final int V7W_VALENCY = 108;
    public static final int V7W_IRAB2 = 109;
    public static final int V7W_P2_IRAB1_SCALE = 110;
    public static final int V7W_P2_SYN_SCALE = 111;

    // V8 LightningBiIrab weights (format.hpp V8WeightId, offset +200)
    public static final int V8W_NEURAL_END = 200;
    public static final int V8W_NEURAL_CRF = 201;
    public static final int V8W_NEURAL_FAM = 202;
    public static final int V8W_NEURAL_TAN = 203;
    public static final int V8W_NEURAL_PRON = 204;
    public static final int V8W_EXP_REFUND = 205;
    public static final int V8W_LEX_END_DAMP = 206;
    public static final int V8W_SYN_KEEP = 207;

    private static final String DEFAULT_ASSET_NAME = "barq.tash";
    private static final String MODEL_CACHE_DIR = "barq_models";
    private static final int IO_BUFFER_BYTES = 65536;

    private final ReentrantReadWriteLock lock = new ReentrantReadWriteLock();
    private volatile long handle;

    /**
     * Loads a {@code .tash} model from a local filesystem path.
     *
     * @param modelPath absolute path to the {@code barq.tash} model file
     * @throws IllegalArgumentException if {@code modelPath} is null or empty
     * @throws IllegalStateException if native loading fails
     */
    public barq(String modelPath) {
        if (modelPath == null || modelPath.trim().isEmpty()) {
            throw new IllegalArgumentException("modelPath must not be null or empty");
        }
        long ptr = nativeCreate(modelPath);
        if (ptr == 0L) {
            throw new IllegalStateException("Failed to load BARQ model: " + modelPath);
        }
        this.handle = ptr;
    }

    /**
     * Loads a {@code .tash} model from a {@link File}.
     *
     * @param modelFile existing model file on disk
     */
    public barq(File modelFile) {
        this(requireFilePath(modelFile));
    }

    private static String requireFilePath(File modelFile) {
        if (modelFile == null) {
            throw new IllegalArgumentException("modelFile must not be null");
        }
        return modelFile.getAbsolutePath();
    }

    /**
     * Opens a BARQ engine from an explicit filesystem path.
     *
     * @param modelPath path to {@code barq.tash}
     * @return initialized {@link barq} instance
     */
    public static barq open(String modelPath) {
        return new barq(modelPath);
    }

    /**
     * Extracts (with size-verified disk caching) {@code barq.tash} from Android {@code assets/}
     * and returns an initialized engine ready for inference.
     *
     * @param context Android Context (Application or Activity)
     * @return initialized {@link barq} instance
     * @throws IOException if extracting the asset fails
     */
    public static barq fromAssets(Context context) throws IOException {
        return fromAssets(context, DEFAULT_ASSET_NAME);
    }

    /**
     * Extracts (with size-verified disk caching) a named model asset from Android {@code assets/}
     * into app-private storage and returns an initialized engine.
     *
     * @param context   Android Context
     * @param assetName asset filename inside {@code assets/} (e.g. {@code "barq.tash"})
     * @return initialized {@link barq} instance
     * @throws IOException if reading or writing the asset fails
     */
    public static barq fromAssets(Context context, String assetName) throws IOException {
        if (context == null) {
            throw new IllegalArgumentException("context must not be null");
        }
        if (assetName == null || assetName.trim().isEmpty()) {
            throw new IllegalArgumentException("assetName must not be null or empty");
        }

        File cacheDir = new File(context.getFilesDir(), MODEL_CACHE_DIR);
        if (!cacheDir.exists() && !cacheDir.mkdirs() && !cacheDir.isDirectory()) {
            throw new IOException("Unable to create model cache directory: " + cacheDir);
        }

        String safeName = new File(assetName).getName();
        File outFile = new File(cacheDir, safeName);
        File sizeMarker = new File(cacheDir, safeName + ".size");
        AssetManager assets = context.getAssets();

        long expectedSize = probeAssetLength(assets, assetName);
        if (isCacheValid(outFile, sizeMarker, expectedSize)) {
            return new barq(outFile);
        }

        File tmpFile = new File(cacheDir, safeName + ".tmp");
        long written = 0L;
        byte[] buffer = new byte[IO_BUFFER_BYTES];
        try (InputStream in = assets.open(assetName);
             FileOutputStream out = new FileOutputStream(tmpFile, false)) {
            int n;
            while ((n = in.read(buffer)) != -1) {
                if (n > 0) {
                    out.write(buffer, 0, n);
                    written += n;
                }
            }
            out.getFD().sync();
        } catch (IOException e) {
            // Best-effort cleanup of incomplete temp file
            //noinspection ResultOfMethodCallIgnored
            tmpFile.delete();
            throw e;
        }

        if (outFile.exists() && !outFile.delete()) {
            //noinspection ResultOfMethodCallIgnored
            tmpFile.delete();
            throw new IOException("Unable to replace cached model file: " + outFile);
        }
        if (!tmpFile.renameTo(outFile)) {
            //noinspection ResultOfMethodCallIgnored
            tmpFile.delete();
            throw new IOException("Unable to move temporary model file into place: " + outFile);
        }
        writeSizeMarker(sizeMarker, written);
        return new barq(outFile);
    }

    private static long probeAssetLength(AssetManager assets, String assetName) {
        try (AssetFileDescriptor afd = assets.openFd(assetName)) {
            return afd.getLength();
        } catch (IOException ignored) {
            // Asset is compressed inside APK; fall back to size marker verification.
            return -1L;
        }
    }

    private static boolean isCacheValid(File modelFile, File sizeMarker, long expectedSize) {
        if (!modelFile.isFile() || modelFile.length() <= 0L) {
            return false;
        }
        if (expectedSize > 0L) {
            return modelFile.length() == expectedSize;
        }
        if (!sizeMarker.isFile()) {
            return false;
        }
        try (InputStream in = new java.io.FileInputStream(sizeMarker)) {
            byte[] buf = new byte[64];
            int n = in.read(buf);
            if (n <= 0) return false;
            long recorded = Long.parseLong(new String(buf, 0, n, StandardCharsets.UTF_8).trim());
            return recorded > 0L && modelFile.length() == recorded;
        } catch (IOException | NumberFormatException ignored) {
            return false;
        }
    }

    private static void writeSizeMarker(File markerFile, long size) {
        try (FileOutputStream out = new FileOutputStream(markerFile, false)) {
            out.write(Long.toString(size).getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException ignored) {
            // Non-fatal: asset extraction itself succeeded.
        }
    }

    private long ensureOpen() {
        long ptr = this.handle;
        if (ptr == 0L) {
            throw new IllegalStateException("BARQ engine has been closed");
        }
        return ptr;
    }

    /**
     * Returns {@code true} if the native engine handle is still open.
     */
    public boolean isOpen() {
        return handle != 0L;
    }

    /**
     * Diacritizes an Arabic UTF-8 string using BARQ (StatCore + LightningBiIrab).
     *
     * @param text input Arabic text (may be partially diacritized or undiacritized)
     * @return fully diacritized text, or {@code text} directly if null/empty
     * @throws IllegalStateException if {@link #close()} has been called
     */
    public String diacritize(String text) {
        if (text == null || text.isEmpty()) {
            return text;
        }
        lock.readLock().lock();
        try {
            long ptr = ensureOpen();
            return nativeDiacritize(ptr, text);
        } finally {
            lock.readLock().unlock();
        }
    }

    /**
     * Diacritizes an array of strings in a single JNI boundary crossing for lower overhead.
     *
     * @param texts array of input strings (null array returns null; null elements stay null)
     * @return array of diacritized strings matching input indices
     * @throws IllegalStateException if {@link #close()} has been called
     */
    public String[] diacritizeBatch(String[] texts) {
        if (texts == null) {
            return null;
        }
        if (texts.length == 0) {
            return new String[0];
        }
        lock.readLock().lock();
        try {
            long ptr = ensureOpen();
            return nativeDiacritizeBatch(ptr, texts);
        } finally {
            lock.readLock().unlock();
        }
    }

    /**
     * Diacritizes a list of lines in a single native batch call.
     *
     * @param lines input lines (may be empty or null)
     * @return immutable-order list of diacritized lines
     */
    public List<String> diacritizeLines(List<String> lines) {
        if (lines == null) {
            return null;
        }
        if (lines.isEmpty()) {
            return Collections.emptyList();
        }
        String[] inArr = lines.toArray(new String[0]);
        String[] outArr = diacritizeBatch(inArr);
        List<String> res = new ArrayList<>(outArr.length);
        Collections.addAll(res, outArr);
        return res;
    }

    /**
     * Removes Arabic diacritics (U+064B..U+0652, U+0670) and tatweel (U+0640) from text.
     *
     * @param text input text
     * @return stripped string, or {@code text} if null/empty
     */
    public static String stripDiacritics(String text) {
        if (text == null || text.isEmpty()) {
            return text;
        }
        int len = text.length();
        StringBuilder sb = new StringBuilder(len);
        for (int i = 0; i < len; i++) {
            char c = text.charAt(i);
            if ((c >= '\u064B' && c <= '\u0652') || c == '\u0670' || c == '\u0640') {
                continue;
            }
            sb.append(c);
        }
        return sb.toString();
    }

    /**
     * Returns the total lexicon vocabulary count stored in the loaded {@code .tash} model.
     */
    public int getVocabSize() {
        lock.readLock().lock();
        try {
            return nativeVocabSize(ensureOpen());
        } finally {
            lock.readLock().unlock();
        }
    }

    /**
     * Returns {@code true} if the active model includes the {@code LightningBiIrab} NRAB neural module.
     */
    public boolean hasNeuralIrab() {
        lock.readLock().lock();
        try {
            return nativeHasNeuralIrab(ensureOpen());
        } finally {
            lock.readLock().unlock();
        }
    }

    /**
     * Reads a decoder/neural weight by ID ({@code W_*}, {@code V7W_*}, or {@code V8W_*}).
     */
    public int getWeight(int id) {
        lock.readLock().lock();
        try {
            return nativeGetWeight(ensureOpen(), id);
        } finally {
            lock.readLock().unlock();
        }
    }

    /**
     * Updates a decoder/neural weight at runtime by ID ({@code W_*}, {@code V7W_*}, or {@code V8W_*}).
     *
     * @return {@code true} if {@code id} is valid
     */
    public boolean setWeight(int id, int value) {
        lock.writeLock().lock();
        try {
            return nativeSetWeight(ensureOpen(), id, value);
        } finally {
            lock.writeLock().unlock();
        }
    }

    /**
     * Enables or disables automatic orthography/spelling normalization hints during decoding.
     */
    public void setRestoreSpelling(boolean enabled) {
        lock.writeLock().lock();
        try {
            nativeSetRestoreSpelling(ensureOpen(), enabled);
        } finally {
            lock.writeLock().unlock();
        }
    }

    /**
     * Releases native memory associated with this engine instance. Safe to call multiple times.
     */
    @Override
    public void close() {
        lock.writeLock().lock();
        try {
            long ptr = this.handle;
            if (ptr != 0L) {
                this.handle = 0L;
                nativeDestroy(ptr);
            }
        } finally {
            lock.writeLock().unlock();
        }
    }

    @SuppressWarnings("deprecation")
    @Override
    protected void finalize() throws Throwable {
        try {
            close();
        } finally {
            super.finalize();
        }
    }

    private static native long nativeCreate(String modelPath);
    private static native void nativeDestroy(long handle);
    private static native String nativeDiacritize(long handle, String text);
    private static native String[] nativeDiacritizeBatch(long handle, String[] texts);
    private static native int nativeVocabSize(long handle);
    private static native boolean nativeHasNeuralIrab(long handle);
    private static native int nativeGetWeight(long handle, int id);
    private static native boolean nativeSetWeight(long handle, int id, int value);
    private static native void nativeSetRestoreSpelling(long handle, boolean enabled);
}
