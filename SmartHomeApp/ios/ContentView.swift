import SwiftUI

// Node-RED server
let baseURL = "http://vanvatcorp.com:1880"

// MARK: - Model

struct LogEntry: Identifiable {
    let id = UUID()
    let date: Date?
    let rawTs: String
    let msg: String
    let isWarn: Bool
}

struct HomeState {
    var link = "offline"
    var mode = "auto"
    var temp: Double?
    var hum: Double?
    var gas: Double?
    var pir = false
    var thTemp = 30
    var thGas = 600
    var fan = false
    var light = false
    var buzzer = false
    var doorOpen = false
    var logs: [LogEntry] = []
}

// MARK: - Store

@MainActor
final class HomeStore: ObservableObject {
    @Published var state = HomeState()
    @Published var error: String?
    private var task: Task<Void, Never>?

    func start() {
        task?.cancel()
        task = Task {
            while !Task.isCancelled {
                await refresh()
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    func stop() {
        task?.cancel()
        task = nil
    }

    func refresh() async {
        guard let url = URL(string: baseURL + "/api/state") else { return }
        var req = URLRequest(url: url)
        req.cachePolicy = .reloadIgnoringLocalCacheData
        req.timeoutInterval = 4
        do {
            let (data, _) = try await URLSession.shared.data(for: req)
            if let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] {
                state = Self.parse(obj)
                error = nil
            }
        } catch {
            self.error = error.localizedDescription
        }
    }

    func post(_ path: String, _ body: [String: Any]) async {
        guard let url = URL(string: baseURL + path) else { return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.timeoutInterval = 4
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: body)
        do {
            _ = try await URLSession.shared.data(for: req)
        } catch {
            self.error = error.localizedDescription
        }
        await refresh()
    }

    func setDevice(_ dev: String, on: Bool) {
        switch dev {
        case "fan": state.fan = on
        case "light": state.light = on
        case "buzzer": state.buzzer = on
        default: break
        }
        Task { await post("/api/cmd/\(dev)", ["state": on ? "on" : "off", "src": "app"]) }
    }

    func openDoor() {
        Task { await post("/api/cmd/door", ["state": "open", "src": "app"]) }
    }

    func setMode(_ m: String) {
        state.mode = m
        Task { await post("/api/config", ["mode": m]) }
    }

    func saveThresholds(temp: Int, gas: Int) {
        Task { await post("/api/config", ["th": ["temp": temp, "gas": gas]]) }
    }

    func clearLogs() {
        state.logs = []
        Task { await post("/api/config", ["clearLog": true]) }
    }

    // MARK: Parsing (tolerant to number / string / bool variations)

    private static func num(_ v: Any?) -> Double? {
        if let n = v as? NSNumber { return n.doubleValue }
        if let s = v as? String { return Double(s) }
        return nil
    }

    private static func truthy(_ v: Any?) -> Bool {
        if let n = v as? NSNumber { return n.doubleValue != 0 }
        if let s = (v as? String)?.lowercased() { return ["1", "true", "on", "yes"].contains(s) }
        return false
    }

    private static func parse(_ o: [String: Any]) -> HomeState {
        var s = HomeState()
        s.link = o["link"] as? String ?? "offline"
        s.mode = o["mode"] as? String ?? "auto"

        let sn = o["sensors"] as? [String: Any] ?? [:]
        s.temp = num(sn["temp"])
        s.hum = num(sn["hum"])
        s.gas = num(sn["gas"])
        s.pir = truthy(sn["pir"])

        let th = o["th"] as? [String: Any] ?? [:]
        s.thTemp = Int(num(th["temp"]) ?? 30)
        s.thGas = Int(num(th["gas"]) ?? 600)

        let d = o["devices"] as? [String: Any] ?? [:]
        s.fan = (d["fan"] as? String) == "on"
        s.light = (d["light"] as? String) == "on"
        s.buzzer = (d["buzzer"] as? String) == "on"
        s.doorOpen = (d["door"] as? String) == "open"
            || (o["door"] as? String) == "open"
            || truthy(o["doorOpen"])

        let logs = o["logs"] as? [[String: Any]] ?? []
        s.logs = logs.map { l in
            let msg = l["msg"] as? String ?? ""
            let level = l["level"] as? String ?? ""
            var date: Date?
            var raw = ""
            if let n = l["ts"] as? NSNumber {
                let v = n.doubleValue
                date = Date(timeIntervalSince1970: v > 1e11 ? v / 1000 : v)
            } else if let str = l["ts"] as? String {
                raw = str
                date = ISO8601DateFormatter().date(from: str)
            }
            let warn = level == "alarm" || level == "warn"
                || msg.contains("FAIL") || msg.contains("GAS")
                || msg.contains("khong duoc ESP xac nhan")
            return LogEntry(date: date, rawTs: raw, msg: msg, isWarn: warn)
        }
        return s
    }
}

// MARK: - UI

struct Card<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.white)
            .clipShape(RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color(red: 0.89, green: 0.91, blue: 0.94), lineWidth: 1))
    }
}

struct SensorCard: View {
    let title: String
    let value: String
    let desc: String
    let color: Color
    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.caption.bold()).foregroundStyle(.secondary)
                Text(value).font(.title.bold()).foregroundStyle(color)
                    .minimumScaleFactor(0.6).lineLimit(1)
                Text(desc).font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

struct DeviceRow: View {
    let icon: String
    let name: String
    let isOn: Bool
    let disabled: Bool
    let onChange: (Bool) -> Void
    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("\(icon) \(name)").font(.subheadline.bold())
                Text(isOn ? "Đang BẬT" : "Đang TẮT")
                    .font(.caption)
                    .foregroundStyle(isOn ? .green : .secondary)
            }
            Spacer()
            Toggle("", isOn: Binding(get: { isOn }, set: onChange))
                .labelsHidden()
                .disabled(disabled)
        }
    }
}

struct ContentView: View {
    @StateObject private var store = HomeStore()
    @State private var tempText = "30"
    @State private var gasText = "600"
    @FocusState private var fieldFocused: Bool

    private var s: HomeState { store.state }
    private var isAuto: Bool { s.mode == "auto" }
    private var gasDanger: Bool { (s.gas ?? 0) >= Double(s.thGas) && (s.gas ?? 0) > 0 }

    private var connText: String {
        switch s.link {
        case "online": return "🟢 ESP8266: Trực tuyến"
        case "sim": return "🟡 Đang ở chế độ Test"
        default: return "⚪ Chờ kết nối ESP8266..."
        }
    }

    private static let timeFmt: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                if gasDanger {
                    Text("🚨 CẢNH BÁO: PHÁT HIỆN RÒ RỈ KHÍ GAS!\nCòi báo động đã kích hoạt, quạt thông gió đang hút khói.")
                        .font(.subheadline.bold())
                        .foregroundStyle(Color(red: 0.6, green: 0.11, blue: 0.11))
                        .padding(14)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color(red: 1, green: 0.95, blue: 0.95))
                        .clipShape(RoundedRectangle(cornerRadius: 14))
                }

                Card {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("🏠 Smart Home").font(.title3.bold())
                        Text("IOT102 - Nhóm 1 • Node Phòng Khách & Bếp")
                            .font(.footnote).foregroundStyle(.secondary)
                        Text(store.error == nil ? connText : "⚠️ Không kết nối được server")
                            .font(.subheadline.bold())
                        Picker("Chế độ", selection: Binding(get: { s.mode }, set: { store.setMode($0) })) {
                            Text("🤖 Tự động").tag("auto")
                            Text("🖐️ Thủ công").tag("manual")
                        }
                        .pickerStyle(.segmented)
                    }
                }

                LazyVGrid(columns: [GridItem(.flexible(), spacing: 12), GridItem(.flexible())], spacing: 12) {
                    SensorCard(title: "🌡️ NHIỆT ĐỘ",
                               value: s.temp.map { String(format: "%.1f °C", $0) } ?? "-- °C",
                               desc: "DHT11", color: .orange)
                    SensorCard(title: "💧 ĐỘ ẨM",
                               value: s.hum.map { String(format: "%.0f %%", $0) } ?? "-- %",
                               desc: "Độ ẩm không khí", color: .blue)
                    SensorCard(title: "KHÍ GAS MQ-2",
                               value: s.gas.map { String(format: "%.0f ppm", $0) } ?? "-- ppm",
                               desc: gasDanger ? "NGUY HIỂM (ngưỡng \(s.thGas))" : "An toàn (ngưỡng \(s.thGas))",
                               color: gasDanger ? .red : .green)
                    SensorCard(title: "🚶 CHUYỂN ĐỘNG",
                               value: s.pir ? "Có người 🏃" : "Phòng trống 💤",
                               desc: "PIR HC-SR505",
                               color: s.pir ? .green : .secondary)
                }

                Card {
                    VStack(alignment: .leading, spacing: 14) {
                        Text("⚡ Điều khiển thiết bị").font(.headline)
                        Text(isAuto ? "Đang ở chế độ Tự động (chuyển sang \"Thủ công\" để tự bật/tắt)"
                                    : "Đang ở chế độ Thủ công (bạn có thể bật/tắt thiết bị)")
                            .font(.caption).foregroundStyle(.secondary)
                        DeviceRow(icon: "🌀", name: "Quạt hút khói / tản nhiệt", isOn: s.fan, disabled: isAuto) {
                            store.setDevice("fan", on: $0)
                        }
                        DeviceRow(icon: "💡", name: "Đèn trần phòng khách", isOn: s.light, disabled: isAuto) {
                            store.setDevice("light", on: $0)
                        }
                        DeviceRow(icon: "🔔", name: "Còi báo động", isOn: s.buzzer, disabled: isAuto) {
                            store.setDevice("buzzer", on: $0)
                        }
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("🚪 Cửa cổng tự động").font(.subheadline.bold())
                                Text(s.doorOpen ? "Cửa đang MỞ 🔓" : "Cửa đang ĐÓNG 🔒")
                                    .font(.caption)
                                    .foregroundStyle(s.doorOpen ? .green : .secondary)
                            }
                            Spacer()
                            Button("Mở cửa") { store.openDoor() }
                                .buttonStyle(.borderedProminent)
                        }
                    }
                }

                Card {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("⚙️ Cài đặt ngưỡng tự động").font(.headline)
                        HStack {
                            Text("Nhiệt độ bật quạt (°C)").font(.subheadline).foregroundStyle(.secondary)
                            Spacer()
                            TextField("30", text: $tempText)
                                .keyboardType(.numberPad)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 90)
                                .focused($fieldFocused)
                        }
                        HStack {
                            Text("Ngưỡng báo động Gas (ppm)").font(.subheadline).foregroundStyle(.secondary)
                            Spacer()
                            TextField("600", text: $gasText)
                                .keyboardType(.numberPad)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 90)
                                .focused($fieldFocused)
                        }
                        Button("💾 Lưu cài đặt") {
                            if let t = Int(tempText), let g = Int(gasText) {
                                store.saveThresholds(temp: t, gas: g)
                            }
                            fieldFocused = false
                        }
                        .buttonStyle(.borderedProminent)
                    }
                }

                Card {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text("📋 Nhật ký ra vào & báo động").font(.headline)
                            Spacer()
                            Button("Xóa") { store.clearLogs() }.font(.footnote)
                        }
                        if s.logs.isEmpty {
                            Text("Chưa có sự kiện nào").font(.footnote).foregroundStyle(.secondary)
                        } else {
                            ForEach(Array(s.logs.suffix(10).reversed())) { l in
                                HStack(alignment: .top, spacing: 10) {
                                    Text(l.date.map { Self.timeFmt.string(from: $0) } ?? l.rawTs)
                                        .font(.caption.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                    Text(l.msg)
                                        .font(.footnote)
                                        .foregroundStyle(l.isWarn ? .red : .primary)
                                }
                            }
                        }
                    }
                }
            }
            .padding(16)
        }
        .background(Color(red: 0.97, green: 0.98, blue: 0.99))
        .preferredColorScheme(.light)
        .scrollDismissesKeyboard(.interactively)
        .onAppear { store.start() }
        .onDisappear { store.stop() }
        .onChange(of: store.state.thTemp) { _, v in if !fieldFocused { tempText = String(v) } }
        .onChange(of: store.state.thGas) { _, v in if !fieldFocused { gasText = String(v) } }
    }
}

#Preview {
    ContentView()
}
