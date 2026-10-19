package com.ib3port.game;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Updates from the app's archive.org page (the real-icon builds, signed with the same key as the GitHub ones, so
 * either kind updates in place and keeps its saves). The page and the file name come from the build (manifest
 * meta-data ib.updatePage and ib.apkName, from android/app-names.local); builds without them never check.
 * The launcher asks check() once per start; when the page has a newer <apkName>-<version>.apk, it offers it,
 * download() fetches it (checked against the page's MD5) and install() hands it to Android's installer, which asks
 * the player to confirm. Nothing about the player is sent: it is a read of the page's public file list.
 */
final class Updater {
    static final class Release {
        final String version, file, md5, notes;
        final long size;

        Release(String version, String file, String md5, long size, String notes) {
            this.version = version;
            this.file = file;
            this.md5 = md5;
            this.size = size;
            this.notes = notes;
        }

        String url(String page) {
            return "https://archive.org/download/" + page + "/" + file;
        }
    }

    interface Progress {
        void at(long done, long total);
    }

    private static final String ACTION_STATUS = "com.ib3port.game.UPDATE_STATUS";

    private Updater() {}

    // "1.9.7" > "1.9.6"; a test build ("1.9.7-test6") is older than the release of the same number.
    static int compare(String a, String b) {
        String[] x = a.split("-", 2)[0].split("\\."), y = b.split("-", 2)[0].split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? num(x[i]) : 0, q = i < y.length ? num(y[i]) : 0;
            if (p != q) return Integer.compare(p, q);
        }
        boolean aTest = a.contains("-"), bTest = b.contains("-");
        return aTest == bTest ? 0 : aTest ? -1 : 1;
    }

    private static int num(String s) {
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(true);  // archive.org/download redirects to a storage server (https -> https)
        c.setRequestProperty("User-Agent", "IB-Android-port-updater");
        if (c.getResponseCode() != 200) throw new IOException("archive.org answered " + c.getResponseCode());
        return c;
    }

    // The newest release on the page if it is newer than `current`, else null. Network: call off the UI thread.
    static Release check(String page, String apkName, String current) throws Exception {
        HttpURLConnection c = open("https://archive.org/metadata/" + page);
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        try (InputStream in = c.getInputStream()) {
            byte[] buf = new byte[1 << 14];
            for (int n; (n = in.read(buf)) > 0; ) body.write(buf, 0, n);
        }
        JSONObject info = new JSONObject(body.toString("UTF-8"));
        Pattern name = Pattern.compile(Pattern.quote(apkName) + "-([0-9][0-9.]*[0-9])\\.apk");
        JSONArray files = info.getJSONArray("files");
        Release best = null;
        for (int i = 0; i < files.length(); i++) {
            JSONObject f = files.getJSONObject(i);
            Matcher m = name.matcher(f.optString("name"));
            if (!m.matches()) continue;
            String v = m.group(1);
            if (best == null || compare(v, best.version) > 0)
                best = new Release(v, f.getString("name"), f.optString("md5"), f.optLong("size", -1), null);
        }
        if (best == null || compare(best.version, current) <= 0) return null;
        return new Release(best.version, best.file, best.md5, best.size, notes(info, best.version));
    }

    // The description's "New: version X: ..." line for that version, as plain text (the descriptions are made by
    // the release scripts, so the line always looks the same); null if it is not there.
    private static String notes(JSONObject page, String version) {
        JSONObject meta = page.optJSONObject("metadata");
        String desc = meta == null ? "" : meta.optString("description");
        Matcher m = Pattern.compile("<p><b>New: version " + Pattern.quote(version) + "</b>:?(.*?)</p>", Pattern.DOTALL).matcher(desc);
        if (!m.find()) return null;
        String text = m.group(1).replaceAll("<[^>]+>", "").replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
                .replace("(see Changelog)", "").replaceAll("\\s+", " ").trim();
        text = text.replaceAll("[\\s.]+$", "");
        return text.isEmpty() ? null : Character.toUpperCase(text.charAt(0)) + text.substring(1) + ".";
    }

    // Downloads the release into the app's cache and checks it; returns the file. Network: off the UI thread.
    static File download(Context context, String page, Release r, Progress progress) throws Exception {
        File dir = new File(context.getCacheDir(), "update");
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) f.delete();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Could not make room for the update.");
        File apk = new File(dir, r.file);
        HttpURLConnection c = open(r.url(page));
        long total = r.size > 0 ? r.size : c.getContentLengthLong();
        MessageDigest md5 = MessageDigest.getInstance("MD5");
        try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(apk)) {
            byte[] buf = new byte[1 << 16];
            long done = 0;
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                md5.update(buf, 0, n);
                done += n;
                progress.at(done, total);
            }
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : md5.digest()) hex.append(String.format("%02x", b));
        if (!r.md5.isEmpty() && !hex.toString().equalsIgnoreCase(r.md5)) {
            apk.delete();
            throw new IOException("The download was damaged. Try again.");
        }
        return apk;
    }

    // Whether Android lets this app install apps; if not, the player switches it on in a settings page.
    static boolean mayInstall(Context context) {
        return context.getPackageManager().canRequestPackageInstalls();
    }

    static Intent installPermissionSettings(Context context) {
        return new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                android.net.Uri.parse("package:" + context.getPackageName()));
    }

    // Hands `apk` to Android's installer. It asks the player to confirm (onConfirm gets the screen to start), then
    // replaces this app, which closes it; onFailure gets a message if it did not install.
    static void install(Activity activity, File apk, java.util.function.Consumer<Intent> onConfirm,
                        java.util.function.Consumer<String> onFailure) throws IOException {
        PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(activity.getPackageName());
        params.setSize(apk.length());
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                session.fsync(out);
            }
            BroadcastReceiver receiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context c, Intent intent) {
                    int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                    if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                        Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                        if (confirm != null) onConfirm.accept(confirm);
                        return;
                    }
                    try {
                        activity.unregisterReceiver(this);
                    } catch (IllegalArgumentException e) {
                        // already gone
                    }
                    if (status != PackageInstaller.STATUS_SUCCESS) {
                        String why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                        onFailure.accept(status == PackageInstaller.STATUS_FAILURE_ABORTED ? "Update cancelled."
                                : "The update did not install" + (why != null ? " (" + why + ")." : "."));
                    }
                }
            };
            IntentFilter filter = new IntentFilter(ACTION_STATUS);
            if (Build.VERSION.SDK_INT >= 33) activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
            else activity.registerReceiver(receiver, filter);
            Intent status = new Intent(ACTION_STATUS).setPackage(activity.getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            session.commit(PendingIntent.getBroadcast(activity, id, status, flags).getIntentSender());
        }
    }

    static String megabytes(long bytes) {
        return String.format(java.util.Locale.US, "%.1f MB", bytes / 1048576.0);
    }
}
