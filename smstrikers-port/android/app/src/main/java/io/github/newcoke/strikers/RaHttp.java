package io.github.newcoke.strikers;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * strikers-quest r46: the HTTPS calls the game's RetroAchievements side makes (src/platform/xr_ra.cpp,
 * rcheevos), through Android's own TLS. Called from that side's worker thread over JNI, one call at a
 * time; each blocks until RetroAchievements answers or the call times out.
 */
final class RaHttp {
    private RaHttp() {}

    /**
     * A POST of {@code body} (a GET when it is null) to {@code url}. Returns the answer's body (null
     * when there is none) and puts its HTTP status in {@code status[0]}, 0 when nothing came back.
     */
    @SuppressWarnings("unused")  // JNI
    static byte[] post(String url, String body, String contentType, String userAgent, int[] status) {
        status[0] = 0;
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(20000);
            c.setRequestProperty("User-Agent", userAgent);
            if (body != null) {
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type",
                        contentType != null ? contentType : "application/x-www-form-urlencoded");
                try (OutputStream out = c.getOutputStream()) {
                    out.write(body.getBytes(StandardCharsets.UTF_8));
                }
            }
            final int code = c.getResponseCode();
            InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream s = in) {
                    byte[] buf = new byte[16384];
                    for (int n; (n = s.read(buf)) > 0; ) bytes.write(buf, 0, n);
                }
            }
            status[0] = code;
            return bytes.toByteArray();
        } catch (IOException | RuntimeException e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
