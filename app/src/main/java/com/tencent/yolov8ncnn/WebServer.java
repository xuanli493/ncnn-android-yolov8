package com.tencent.yolov8ncnn;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.HashMap;

public class WebServer extends Thread
{
    private static final String TAG = "WebServer";

    private final YOLOv8Ncnn ncnn;
    private final AssetManager assets;
    private ServerSocket serverSocket;
    private volatile boolean running = true;

    private byte[] htmlBytes;

    private Bitmap bitmap;
    private int bitmapW = 0;
    private int bitmapH = 0;

    public WebServer(YOLOv8Ncnn ncnn, AssetManager assets)
    {
        this.ncnn = ncnn;
        this.assets = assets;
        loadHtml();
    }

    private void loadHtml()
    {
        try {
            InputStream is = assets.open("index.html");
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = is.read(buf)) != -1) {
                baos.write(buf, 0, n);
            }
            is.close();
            htmlBytes = baos.toByteArray();
        } catch (IOException e) {
            Log.e(TAG, "load index.html failed", e);
            htmlBytes = "<html><body>error</body></html>".getBytes();
        }
    }

    public void shutdown()
    {
        running = false;
        try {
            if (serverSocket != null) serverSocket.close();
        } catch (IOException e) {
            // ignore
        }
    }

    @Override
    public void run()
    {
        try {
            serverSocket = new ServerSocket(8080);
        } catch (IOException e) {
            Log.e(TAG, "bind 8080 failed", e);
            return;
        }

        Log.i(TAG, "server started on 8080");

        while (running)
        {
            try {
                Socket socket = serverSocket.accept();
                new Thread(new ConnectionHandler(socket)).start();
            } catch (IOException e) {
                if (running) Log.e(TAG, "accept", e);
            }
        }
    }

    private class ConnectionHandler implements Runnable
    {
        private final Socket socket;

        ConnectionHandler(Socket s)
        {
            this.socket = s;
        }

        @Override
        public void run()
        {
            try {
                InputStream in = socket.getInputStream();
                OutputStream out = socket.getOutputStream();

                String requestLine = readLine(in);
                if (requestLine == null || requestLine.isEmpty()) {
                    socket.close();
                    return;
                }

                drainHeaders(in);

                String[] parts = requestLine.split(" ");
                if (parts.length < 2) {
                    socket.close();
                    return;
                }
                String pathQuery = parts[1];

                if (pathQuery.equals("/") || pathQuery.startsWith("/index")) {
                    sendHtml(out);
                } else if (pathQuery.startsWith("/stream")) {
                    sendStream(out);
                } else if (pathQuery.startsWith("/api/")) {
                    handleApi(pathQuery, out);
                } else {
                    send404(out);
                }
            } catch (IOException e) {
                // ignore client disconnect
            } finally {
                try { socket.close(); } catch (IOException e) {}
            }
        }
    }

    // ===== HTTP helpers =====

    private String readLine(InputStream in) throws IOException
    {
        StringBuilder sb = new StringBuilder();
        int c;
        while ((c = in.read()) != -1 && c != '\n') {
            if (c != '\r') sb.append((char) c);
        }
        return sb.toString();
    }

    private void drainHeaders(InputStream in) throws IOException
    {
        String line;
        while ((line = readLine(in)) != null && line.length() > 0) {
            // skip header line
        }
    }

    private void sendHtml(OutputStream out) throws IOException
    {
        out.write(("HTTP/1.1 200 OK\r\n" +
                   "Content-Type: text/html; charset=utf-8\r\n" +
                   "Content-Length: " + htmlBytes.length + "\r\n" +
                   "Connection: close\r\n\r\n").getBytes("UTF-8"));
        out.write(htmlBytes);
        out.flush();
    }

    private void send404(OutputStream out) throws IOException
    {
        byte[] body = "404".getBytes("UTF-8");
        out.write(("HTTP/1.1 404 Not Found\r\n" +
                   "Content-Length: " + body.length + "\r\n" +
                   "Connection: close\r\n\r\n").getBytes("UTF-8"));
        out.write(body);
        out.flush();
    }

    private void sendStream(OutputStream out) throws IOException
    {
        out.write(("HTTP/1.1 200 OK\r\n" +
                   "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n" +
                   "Cache-Control: no-cache\r\n" +
                   "Connection: close\r\n\r\n").getBytes("UTF-8"));
        out.flush();

        while (running)
        {
            byte[] jpeg = captureJpeg();
            if (jpeg == null) {
                try { Thread.sleep(30); } catch (InterruptedException e) { break; }
                continue;
            }
            String part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + jpeg.length + "\r\n\r\n";
            out.write(part.getBytes("UTF-8"));
            out.write(jpeg);
            out.write("\r\n".getBytes("UTF-8"));
            out.flush();
            try { Thread.sleep(33); } catch (InterruptedException e) { break; }
        }
    }

    private void handleApi(String pathQuery, OutputStream out) throws IOException
    {
        String path = pathQuery;
        String query = "";
        int q = pathQuery.indexOf('?');
        if (q >= 0) {
            path = pathQuery.substring(0, q);
            query = pathQuery.substring(q + 1);
        }
        HashMap<String, String> p = parseQuery(query);

        String result = "ok";

        if (path.startsWith("/api/cameras")) {
            result = ncnn.listCameras(parseInt(p.get("facing"), 1));
        } else if (path.startsWith("/api/camera")) {
            boolean ok = ncnn.openCameraIndex(parseInt(p.get("facing"), 1), parseInt(p.get("index"), 0));
            result = ok ? "ok" : "err";
        } else if (path.startsWith("/api/af")) {
            ncnn.setAFMode(parseInt(p.get("mode"), 3));
        } else if (path.startsWith("/api/focus")) {
            ncnn.setFocusDistance(parseFloat(p.get("d"), 0f));
        } else if (path.startsWith("/api/ae")) {
            ncnn.setAEMode(parseInt(p.get("mode"), 1));
        } else if (path.startsWith("/api/exposure")) {
            ncnn.setExposureTime(parseLong(p.get("ns"), 0L));
        } else if (path.startsWith("/api/iso")) {
            ncnn.setSensitivity(parseInt(p.get("value"), 100));
        } else if (path.startsWith("/api/awb")) {
            ncnn.setAwbMode(parseInt(p.get("mode"), 1));
        } else if (path.startsWith("/api/reset")) {
            ncnn.setAFMode(3);
            ncnn.setAEMode(1);
            ncnn.setAwbMode(1);
        } else if (path.startsWith("/api/boxes")) {
            ncnn.setDrawBoxes(parseInt(p.get("on"), 0) != 0);
        } else {
            result = "unknown";
        }

        byte[] body = result.getBytes("UTF-8");
        out.write(("HTTP/1.1 200 OK\r\n" +
                   "Content-Type: text/plain; charset=utf-8\r\n" +
                   "Access-Control-Allow-Origin: *\r\n" +
                   "Content-Length: " + body.length + "\r\n" +
                   "Connection: close\r\n\r\n").getBytes("UTF-8"));
        out.write(body);
        out.flush();
    }

    private HashMap<String, String> parseQuery(String query)
    {
        HashMap<String, String> map = new HashMap<>();
        if (query == null || query.isEmpty()) return map;
        String[] pairs = query.split("&");
        for (String pair : pairs) {
            int eq = pair.indexOf('=');
            if (eq > 0) {
                map.put(pair.substring(0, eq), pair.substring(eq + 1));
            }
        }
        return map;
    }

    private int parseInt(String s, int def)
    {
        try { return Integer.parseInt(s); } catch (Exception e) { return def; }
    }

    private long parseLong(String s, long def)
    {
        try { return Long.parseLong(s); } catch (Exception e) { return def; }
    }

    private float parseFloat(String s, float def)
    {
        try { return Float.parseFloat(s); } catch (Exception e) { return def; }
    }

    // ===== 帧 → JPEG =====

    private byte[] captureJpeg()
    {
        int w = ncnn.getFrameWidth();
        int h = ncnn.getFrameHeight();
        if (w <= 0 || h <= 0) return null;

        byte[] rgb = ncnn.getFrameRGB();
        if (rgb == null || rgb.length < w * h * 3) return null;

        if (bitmap == null || bitmapW != w || bitmapH != h) {
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            bitmapW = w;
            bitmapH = h;
        }

        int[] pixels = new int[w * h];
        for (int i = 0, j = 0; i < pixels.length; i++, j += 3) {
            int r = rgb[j] & 0xFF;
            int g = rgb[j + 1] & 0xFF;
            int b = rgb[j + 2] & 0xFF;
            pixels[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
        bitmap.setPixels(pixels, 0, w, 0, 0, w, h);

        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        bitmap.compress(Bitmap.CompressFormat.JPEG, 80, baos);
        return baos.toByteArray();
    }
}
