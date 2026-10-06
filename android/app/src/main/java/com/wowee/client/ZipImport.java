package com.wowee.client;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.format.Formatter;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * Unpacks a zip of extracted game data into the data folder.
 *
 * The alternative to copying a Data folder over file by file: the player zips
 * it on the PC, puts the one file on the phone however is easiest, and the app
 * unpacks it into the folder the client reads (DataFolderActivity.chosenFolder,
 * /sdcard/wowee by default).
 *
 * Everything is unpacked into a staging folder inside the target first, and
 * only moved into place once the whole zip has come out and holds a manifest.
 * So a cancel, a failure or a killed process never leaves half a data set where
 * the client would find it; the staging folder is simply deleted, and one left
 * behind by a killed process is deleted on the next start (cleanStale).
 *
 * Finding the manifest in staging, rather than assuming the zip's layout, is
 * also what copes with how people zip a folder: the contents at the top, a Data/
 * folder, or a wrapper around that ("wowee/Data/manifest.json").
 *
 * There is one import at a time and it belongs to the process, not to an
 * Activity, so the screen can be recreated under it and pick it up again.
 * It is not a foreground service: leaving the app for long enough can get the
 * process killed, which ends the import the same way a cancel does.
 */
final class ZipImport {

    interface Listener {
        void onImportChanged(ZipImport job);
    }

    enum State { RUNNING, DONE, FAILED, CANCELLED }

    private static final String TAG = "Wowee";
    /** Hidden, so it is not mistaken for data while it is being filled. */
    static final String STAGING = ".wowee-zip-import";
    /** Written beside the data once an import is complete. */
    static final String MARKER = ".wowee-zip-imported";
    /** What a well-known zip is called; see DataFolderActivity.wellKnownZip. */
    static final String WELL_KNOWN_NAME = "wowee-data.zip";
    /** Headroom beyond the data itself, so the device is not filled to the byte. */
    private static final long SPACE_MARGIN = 256L * 1024 * 1024;
    private static final long NOTIFY_INTERVAL_MS = 100;

    private static ZipImport current;

    private final Context app;
    private final Handler main = new Handler(Looper.getMainLooper());
    final String sourceName;
    private final File sourceFile;
    private final Uri sourceUri;
    private final long sourceSize;
    final File target;
    private final File staging;

    // Written by the worker, read by the UI thread.
    private volatile State state = State.RUNNING;
    private volatile boolean cancelRequested;
    private volatile boolean finishing;
    private volatile long done;
    private volatile long total = -1;
    private volatile String entry = "";
    private volatile String error;

    private Listener listener;
    private long lastNotify;

    /** The import in progress, or finished and not yet seen by the screen. */
    static synchronized ZipImport current() {
        return current;
    }

    /** Forgets a finished import once the screen has acted on it. */
    static synchronized void clear(ZipImport job) {
        if (current == job && job.state != State.RUNNING) current = null;
    }

    /** Unpacks a zip the app can open by path, which gives exact sizes. */
    static synchronized ZipImport start(Context context, File zip, File target) {
        return begin(new ZipImport(context, zip.getName(), zip, null, zip.length(), target));
    }

    /**
     * Unpacks a zip the storage access framework handed over. size is the
     * provider's OpenableColumns.SIZE, or -1 when it did not say.
     */
    static synchronized ZipImport start(Context context, Uri zip, String name, long size,
                                        File target) {
        return begin(new ZipImport(context, name, null, zip, size, target));
    }

    private static ZipImport begin(ZipImport job) {
        if (current != null && current.state == State.RUNNING) {
            throw new IllegalStateException("an import is already running");
        }
        current = job;
        Thread worker = new Thread(job::run, "wowee-zip-import");
        worker.start();
        return job;
    }

    /** Removes a staging folder an import killed with the process left behind. */
    static void cleanStale(File target) {
        synchronized (ZipImport.class) {
            if (current != null && current.state == State.RUNNING) return;
        }
        File stale = new File(target, STAGING);
        if (!stale.exists()) return;
        // Renamed out of the way first, so an import started while it is
        // still being deleted gets a staging folder of its own.
        File doomed = new File(target, STAGING + "-stale-" + System.currentTimeMillis());
        File gone = stale.renameTo(doomed) ? doomed : stale;
        new Thread(() -> deleteTree(gone), "wowee-zip-cleanup").start();
    }

    private ZipImport(Context context, String name, File file, Uri uri, long size, File target) {
        this.app = context.getApplicationContext();
        this.sourceName = name;
        this.sourceFile = file;
        this.sourceUri = uri;
        this.sourceSize = size;
        this.target = target;
        this.staging = new File(target, STAGING);
    }

    State state() { return state; }
    boolean cancelRequested() { return cancelRequested; }
    /** True once unpacking is over and the files are being moved into place. */
    boolean finishing() { return finishing; }
    long done() { return done; }
    /** What done counts towards, or -1 when that is not known. */
    long total() { return total; }
    String entry() { return entry; }
    String error() { return error; }

    void cancel() {
        cancelRequested = true;
        post(true);
    }

    /** Set from onResume, cleared from onPause; called on the main thread. */
    void setListener(Listener l) {
        listener = l;
        if (l != null) l.onImportChanged(this);
    }

    private void run() {
        try {
            deleteTree(staging);
            if (!staging.mkdirs()) throw new IOException("could not create " + staging);
            if (sourceFile != null) {
                unpackFile();
            } else {
                unpackStream();
            }

            File root = findDataRoot(staging);
            if (root == null) throw new IOException(app.getString(R.string.zip_no_data));

            if (cancelRequested) throw new CancelledException();
            // Past the point where cancelling helps: the moves are renames and
            // quick, and stopping half way would leave a mixed folder.
            finishing = true;
            post(true);
            moveInto(root, target);
            deleteTree(staging);
            writeMarker();
            finish(State.DONE, null);
        } catch (CancelledException e) {
            deleteTree(staging);
            finish(State.CANCELLED, null);
        } catch (IOException | RuntimeException e) {
            Log.e(TAG, "zip import of " + sourceName + " failed", e);
            deleteTree(staging);
            String message = e.getMessage();
            finish(State.FAILED, message != null ? message : e.getClass().getSimpleName());
        }
    }

    /**
     * A zip with a path: the central directory gives every entry's size up
     * front, so progress is by bytes written out of the exact total, and the
     * free space check is exact.
     */
    private void unpackFile() throws IOException {
        try (ZipFile zip = new ZipFile(sourceFile)) {
            long sum = 0;
            boolean known = true;
            for (Enumeration<? extends ZipEntry> it = zip.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                if (e.isDirectory()) continue;
                if (e.getSize() < 0) known = false; else sum += e.getSize();
            }
            total = known ? sum : -1;
            checkSpace(known ? sum : sourceFile.length());

            for (Enumeration<? extends ZipEntry> it = zip.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                File out = resolve(e.getName());
                if (out == null) continue;
                if (e.isDirectory()) {
                    mkdirs(out);
                    continue;
                }
                try (InputStream in = zip.getInputStream(e)) {
                    copy(in, out, null);
                }
            }
        }
    }

    /**
     * A zip only the storage access framework can open: read once, front to
     * back, since entry sizes there may only follow the data. Progress is by
     * bytes read from the zip out of its size, and the space check can only
     * ask for the zip's own size, which is a floor.
     */
    private void unpackStream() throws IOException {
        ContentResolver resolver = app.getContentResolver();
        InputStream raw = resolver.openInputStream(sourceUri);
        if (raw == null) throw new IOException(app.getString(R.string.zip_unreadable));
        CountingInputStream counted = new CountingInputStream(raw);
        total = sourceSize > 0 ? sourceSize : -1;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(counted, 256 * 1024))) {
            if (sourceSize > 0) checkSpace(sourceSize);
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                File out = resolve(e.getName());
                if (out == null) continue;
                if (e.isDirectory()) {
                    mkdirs(out);
                } else {
                    copy(zip, out, counted);
                }
                zip.closeEntry();
            }
        }
    }

    private void checkSpace(long needed) throws IOException {
        long free = staging.getUsableSpace();
        if (free < needed + SPACE_MARGIN) {
            throw new IOException(app.getString(R.string.zip_no_space,
                    target.getAbsolutePath(),
                    Formatter.formatShortFileSize(app, needed + SPACE_MARGIN),
                    Formatter.formatShortFileSize(app, free)));
        }
    }

    /**
     * Where an entry goes under staging, or null for one to skip. Refuses any
     * name that would land outside it - an absolute path, a drive letter or a
     * ".." segment - rather than trusting the zip ("zip slip"). Nothing here
     * creates links, so checking the name is enough.
     */
    private File resolve(String name) throws IOException {
        String n = name.replace('\\', '/');
        if (n.startsWith("/") || (n.length() > 1 && n.charAt(1) == ':')) {
            throw new IOException(app.getString(R.string.zip_bad_entry, name));
        }
        List<String> parts = new ArrayList<>();
        for (String part : n.split("/")) {
            if (part.isEmpty() || part.equals(".")) continue;
            if (part.equals("..")) throw new IOException(app.getString(R.string.zip_bad_entry, name));
            parts.add(part);
        }
        if (parts.isEmpty()) return null;
        // What the macOS Finder adds to every zip it makes; not data.
        if (parts.get(0).equals("__MACOSX") || parts.get(parts.size() - 1).equals(".DS_Store")) {
            return null;
        }
        return new File(staging, String.join("/", parts));
    }

    /**
     * Writes one entry. counted is the zip's own stream when progress is
     * measured on the input; otherwise progress is the bytes written.
     */
    private void copy(InputStream in, File out, CountingInputStream counted) throws IOException {
        entry = out.getAbsolutePath().substring(staging.getAbsolutePath().length() + 1);
        File parent = out.getParentFile();
        if (parent != null) mkdirs(parent);
        byte[] buffer = new byte[256 * 1024];
        try (OutputStream os = new FileOutputStream(out)) {
            int read;
            while ((read = in.read(buffer)) != -1) {
                if (cancelRequested) throw new CancelledException();
                os.write(buffer, 0, read);
                done = counted != null ? counted.count : done + read;
                post(false);
            }
        }
        if (cancelRequested) throw new CancelledException();
        post(false);
    }

    private static void mkdirs(File dir) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create " + dir);
    }

    /**
     * The shallowest folder in staging the client would take for a data root,
     * looking three levels down: covers the contents zipped directly, a Data/
     * folder, and a folder around that.
     */
    private static File findDataRoot(File staging) {
        List<File> level = new ArrayList<>();
        level.add(staging);
        for (int depth = 0; depth <= 3 && !level.isEmpty(); depth++) {
            List<File> next = new ArrayList<>();
            for (File dir : level) {
                if (DataFolderActivity.hasGameData(dir)) return dir;
                File[] children = dir.listFiles(File::isDirectory);
                if (children == null) continue;
                Arrays.sort(children);
                next.addAll(Arrays.asList(children));
            }
            level = next;
        }
        return null;
    }

    /**
     * Moves everything under from into to, merging folders that exist in both
     * and replacing files the zip also has. The manifests go last, so a move
     * that fails part way leaves nothing the client mistakes for a data set.
     */
    private void moveInto(File from, File to) throws IOException {
        mkdirs(to);
        File[] children = from.listFiles();
        if (children == null) throw new IOException("could not list " + from);
        Arrays.sort(children, (a, b) -> Boolean.compare(isManifest(a), isManifest(b)));
        for (File child : children) {
            File dest = new File(to, child.getName());
            if (child.isDirectory() && dest.isDirectory()) {
                moveInto(child, dest);
                continue;
            }
            if (dest.exists()) deleteTree(dest);
            if (!child.renameTo(dest)) throw new IOException("could not move " + child + " to " + dest);
        }
    }

    private static boolean isManifest(File f) {
        return f.getName().equals("manifest.json") || f.getName().equals("expansions");
    }

    private void writeMarker() {
        try (OutputStream out = new FileOutputStream(new File(target, MARKER))) {
            out.write((sourceName + "\n" + System.currentTimeMillis() + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            // The data is in place either way; the marker only records where from.
            Log.w(TAG, "could not write " + MARKER, e);
        }
    }

    static void deleteTree(File f) {
        File[] children = f.isDirectory() ? f.listFiles() : null;
        if (children != null) {
            for (File c : children) deleteTree(c);
        }
        f.delete();
    }

    private void finish(State s, String message) {
        error = message;
        state = s;
        post(true);
    }

    /** Tells the listener on the main thread, at most every NOTIFY_INTERVAL_MS unless forced. */
    private void post(boolean force) {
        long now = SystemClock.uptimeMillis();
        if (!force && now - lastNotify < NOTIFY_INTERVAL_MS) return;
        lastNotify = now;
        main.post(() -> {
            if (listener != null) listener.onImportChanged(this);
        });
    }

    private static final class CancelledException extends RuntimeException {
        private static final long serialVersionUID = 1L;
    }

    /** Counts what is read from the zip itself, for progress by input. */
    private static final class CountingInputStream extends FilterInputStream {
        volatile long count;

        CountingInputStream(InputStream in) {
            super(in);
        }

        @Override
        public int read() throws IOException {
            int b = super.read();
            if (b >= 0) count++;
            return b;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            int n = super.read(b, off, len);
            if (n > 0) count += n;
            return n;
        }

        @Override
        public long skip(long n) throws IOException {
            long s = super.skip(n);
            if (s > 0) count += s;
            return s;
        }
    }
}
