// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The newest version published on GitHub. MyTether only asks when someone clicks Check for
/// Updates, and sends nothing but the request itself.
struct Release {
    var version: String
    var page: URL

    enum Check {
        case newer(Release)
        case upToDate
        case failed
    }

    static let repository = "camembert-picante/MyTether"

    static var current: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    static func check() async -> Check {
        let url = URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!
        var request = URLRequest(url: url, timeoutInterval: 15)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        // Ephemeral: no cookies, and no cached answer from an earlier check.
        let session = URLSession(configuration: .ephemeral)
        defer { session.finishTasksAndInvalidate() }
        guard let (data, response) = try? await session.data(for: request),
              let status = (response as? HTTPURLResponse)?.statusCode else { return .failed }
        // GitHub answers 404 until the first release is published.
        if status == 404 { return .upToDate }
        guard status == 200,
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tag = json["tag_name"] as? String,
              let page = (json["html_url"] as? String).flatMap(URL.init(string:)),
              page.scheme == "https", page.host == "github.com" else { return .failed }
        let latest = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
        guard latest.compare(current, options: .numeric) == .orderedDescending else { return .upToDate }
        return .newer(Release(version: latest, page: page))
    }
}
