package com.example.smarthome;

import android.content.res.ColorStateList;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.appcompat.widget.SwitchCompat;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity {

    // Node-RED server
    private static final String BASE = "http://vanvatcorp.com:1880";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newCachedThreadPool();
    private final SimpleDateFormat timeFmt = new SimpleDateFormat("HH:mm:ss", Locale.getDefault());
    private boolean updating = false;

    private TextView tvBanner, tvConn, tvTemp, tvHum, tvGas, tvGasDesc, tvPir, tvHint;
    private TextView tvFanSt, tvLightSt, tvBuzzerSt, tvDoorSt;
    private SwitchCompat swFan, swLight, swBuzzer;
    private Button btnAuto, btnManual, btnDoor, btnSave, btnClear;
    private EditText etTemp, etGas;
    private LinearLayout logList;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            fetchState();
            handler.postDelayed(this, 2000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_NO);
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvBanner = findViewById(R.id.tvBanner);
        tvConn = findViewById(R.id.tvConn);
        tvTemp = findViewById(R.id.tvTemp);
        tvHum = findViewById(R.id.tvHum);
        tvGas = findViewById(R.id.tvGas);
        tvGasDesc = findViewById(R.id.tvGasDesc);
        tvPir = findViewById(R.id.tvPir);
        tvHint = findViewById(R.id.tvHint);
        tvFanSt = findViewById(R.id.tvFanSt);
        tvLightSt = findViewById(R.id.tvLightSt);
        tvBuzzerSt = findViewById(R.id.tvBuzzerSt);
        tvDoorSt = findViewById(R.id.tvDoorSt);
        swFan = findViewById(R.id.swFan);
        swLight = findViewById(R.id.swLight);
        swBuzzer = findViewById(R.id.swBuzzer);
        btnAuto = findViewById(R.id.btnAuto);
        btnManual = findViewById(R.id.btnManual);
        btnDoor = findViewById(R.id.btnDoor);
        btnSave = findViewById(R.id.btnSave);
        btnClear = findViewById(R.id.btnClear);
        etTemp = findViewById(R.id.etTemp);
        etGas = findViewById(R.id.etGas);
        logList = findViewById(R.id.logList);

        swFan.setOnCheckedChangeListener((b, on) -> { if (!updating) sendCmd("fan", on ? "on" : "off"); });
        swLight.setOnCheckedChangeListener((b, on) -> { if (!updating) sendCmd("light", on ? "on" : "off"); });
        swBuzzer.setOnCheckedChangeListener((b, on) -> { if (!updating) sendCmd("buzzer", on ? "on" : "off"); });
        btnDoor.setOnClickListener(v -> sendCmd("door", "open"));
        btnAuto.setOnClickListener(v -> sendConfig(json("mode", "auto")));
        btnManual.setOnClickListener(v -> sendConfig(json("mode", "manual")));
        btnClear.setOnClickListener(v -> {
            sendConfig(json("clearLog", true));
            logList.removeAllViews();
            logList.addView(logRow("Đã xóa sạch nhật ký", false));
        });
        btnSave.setOnClickListener(v -> saveSettings());
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(poll);
    }

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(poll);
    }

    // ---------- Network ----------

    private String http(String method, String path, String body) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(BASE + path).openConnection();
        try {
            c.setRequestMethod(method);
            c.setConnectTimeout(4000);
            c.setReadTimeout(4000);
            c.setUseCaches(false);
            if (body != null) {
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/json");
                try (OutputStream os = c.getOutputStream()) {
                    os.write(body.getBytes("UTF-8"));
                }
            }
            InputStream in = c.getResponseCode() >= 400 ? c.getErrorStream() : c.getInputStream();
            StringBuilder sb = new StringBuilder();
            if (in != null) {
                try (BufferedReader r = new BufferedReader(new InputStreamReader(in, "UTF-8"))) {
                    String line;
                    while ((line = r.readLine()) != null) sb.append(line);
                }
            }
            return sb.toString();
        } finally {
            c.disconnect();
        }
    }

    private void fetchState() {
        io.execute(() -> {
            try {
                JSONObject s = new JSONObject(http("GET", "/api/state", null));
                runOnUiThread(() -> render(s));
            } catch (Exception e) {
                runOnUiThread(() -> {
                    tvConn.setText("⚠️ Không kết nối được server");
                    tvConn.setTextColor(0xFFB91C1C);
                });
            }
        });
    }

    private void post(String path, JSONObject body) {
        io.execute(() -> {
            try {
                http("POST", path, body.toString());
            } catch (Exception e) {
                runOnUiThread(() -> Toast.makeText(this, "Lỗi gửi lệnh: " + e.getMessage(), Toast.LENGTH_SHORT).show());
            }
            fetchState();
        });
    }

    private void sendCmd(String dev, String state) {
        JSONObject b = new JSONObject();
        try {
            b.put("state", state);
            b.put("src", "app");
        } catch (Exception ignored) { }
        post("/api/cmd/" + dev, b);
    }

    private void sendConfig(JSONObject b) {
        post("/api/config", b);
    }

    private void saveSettings() {
        try {
            JSONObject th = new JSONObject();
            th.put("temp", Integer.parseInt(etTemp.getText().toString().trim()));
            th.put("gas", Integer.parseInt(etGas.getText().toString().trim()));
            sendConfig(json("th", th));
            Toast.makeText(this, "✓ Đã lưu cài đặt!", Toast.LENGTH_SHORT).show();
            etTemp.clearFocus();
            etGas.clearFocus();
        } catch (Exception e) {
            Toast.makeText(this, "Giá trị không hợp lệ", Toast.LENGTH_SHORT).show();
        }
    }

    private static JSONObject json(String k, Object v) {
        JSONObject o = new JSONObject();
        try {
            o.put(k, v);
        } catch (Exception ignored) { }
        return o;
    }

    // ---------- Render ----------

    private void render(JSONObject s) {
        String link = s.optString("link", "");
        if ("online".equals(link)) {
            tvConn.setText("🟢 ESP8266: Trực tuyến");
            tvConn.setTextColor(0xFF15803D);
        } else if ("sim".equals(link)) {
            tvConn.setText("🟡 Đang ở chế độ Test");
            tvConn.setTextColor(0xFFA16207);
        } else {
            tvConn.setText("⚪ Chờ kết nối ESP8266...");
            tvConn.setTextColor(0xFF64748B);
        }

        boolean auto = "auto".equals(s.optString("mode", "auto"));
        styleModeBtn(btnAuto, auto);
        styleModeBtn(btnManual, !auto);
        tvHint.setText(auto
                ? "Đang ở chế độ Tự động (chuyển sang \"Thủ công\" để tự bật/tắt)"
                : "Đang ở chế độ Thủ công (bạn có thể bật/tắt thiết bị)");

        JSONObject sn = s.optJSONObject("sensors");
        if (sn == null) sn = new JSONObject();
        tvTemp.setText(sn.isNull("temp") ? "-- °C" : String.format(Locale.US, "%.1f °C", sn.optDouble("temp")));
        tvHum.setText(sn.isNull("hum") ? "-- %" : String.format(Locale.US, "%.0f %%", sn.optDouble("hum")));

        JSONObject th = s.optJSONObject("th");
        int gasTh = th != null ? th.optInt("gas", 600) : 600;
        double gasVal = sn.isNull("gas") ? 0 : sn.optDouble("gas", 0);
        tvGas.setText(sn.isNull("gas") ? "-- ppm" : String.format(Locale.US, "%.0f ppm", gasVal));
        boolean danger = gasVal >= gasTh && gasVal > 0;
        tvBanner.setVisibility(danger ? View.VISIBLE : View.GONE);
        tvGas.setTextColor(danger ? 0xFFDC2626 : 0xFF16A34A);
        tvGasDesc.setText(danger ? "NGUY HIỂM (ngưỡng " + gasTh + ")" : "An toàn (ngưỡng " + gasTh + ")");

        if (th != null) {
            if (!etTemp.hasFocus() && th.has("temp")) etTemp.setText(String.valueOf(th.optInt("temp")));
            if (!etGas.hasFocus() && th.has("gas")) etGas.setText(String.valueOf(th.optInt("gas")));
        }

        boolean pir = truthy(sn.opt("pir"));
        tvPir.setText(pir ? "Có người 🏃" : "Phòng trống 💤");
        tvPir.setTextColor(pir ? 0xFF16A34A : 0xFF64748B);

        JSONObject d = s.optJSONObject("devices");
        if (d == null) d = new JSONObject();
        setDev(swFan, tvFanSt, "on".equals(d.optString("fan")), auto);
        setDev(swLight, tvLightSt, "on".equals(d.optString("light")), auto);
        setDev(swBuzzer, tvBuzzerSt, "on".equals(d.optString("buzzer")), auto);

        boolean open = "open".equals(d.optString("door")) || "open".equals(s.optString("door"))
                || s.optBoolean("doorOpen", false);
        tvDoorSt.setText(open ? "Cửa đang MỞ 🔓" : "Cửa đang ĐÓNG 🔒");
        tvDoorSt.setTextColor(open ? 0xFF16A34A : 0xFF64748B);

        renderLogs(s.optJSONArray("logs"));
    }

    private void setDev(SwitchCompat sw, TextView st, boolean on, boolean auto) {
        updating = true;
        sw.setChecked(on);
        updating = false;
        sw.setEnabled(!auto);
        st.setText(on ? "Đang BẬT" : "Đang TẮT");
        st.setTextColor(on ? 0xFF16A34A : 0xFF64748B);
    }

    private void styleModeBtn(Button b, boolean active) {
        b.setBackgroundTintList(ColorStateList.valueOf(active ? 0xFF2563EB : 0xFFE2E8F0));
        b.setTextColor(active ? 0xFFFFFFFF : 0xFF1E293B);
    }

    private void renderLogs(JSONArray logs) {
        if (logs == null || logs.length() == 0) return;
        logList.removeAllViews();
        int stop = Math.max(0, logs.length() - 10);
        for (int i = logs.length() - 1; i >= stop; i--) {
            JSONObject l = logs.optJSONObject(i);
            if (l == null) continue;
            String msg = l.optString("msg", "");
            String level = l.optString("level", "");
            boolean warn = "alarm".equals(level) || "warn".equals(level)
                    || msg.contains("FAIL") || msg.contains("GAS") || msg.contains("khong duoc ESP xac nhan");
            Object ts = l.opt("ts");
            String time;
            if (ts instanceof Number) {
                long v = ((Number) ts).longValue();
                if (v < 100000000000L) v *= 1000;
                time = timeFmt.format(new Date(v));
            } else {
                time = String.valueOf(ts);
            }
            logList.addView(logRow(time + "   " + msg, warn));
        }
    }

    private TextView logRow(String text, boolean warn) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(13);
        t.setTextColor(warn ? 0xFFB91C1C : 0xFF1E293B);
        int p = (int) (6 * getResources().getDisplayMetrics().density);
        t.setPadding(0, p, 0, p);
        return t;
    }

    private static boolean truthy(Object v) {
        if (v == null || v == JSONObject.NULL) return false;
        if (v instanceof Boolean) return (Boolean) v;
        if (v instanceof Number) return ((Number) v).doubleValue() != 0;
        String s = String.valueOf(v).toLowerCase(Locale.ROOT);
        return s.equals("1") || s.equals("true") || s.equals("on") || s.equals("yes");
    }
}
