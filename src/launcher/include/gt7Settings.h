#ifndef LAUNCHER_INCLUDE_GT7_SETTINGS_H_
#define LAUNCHER_INCLUDE_GT7_SETTINGS_H_

#include "configuration.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

inline bool IsGt7Title(const Configuration& info) {
	return info.title_id.compare(QStringLiteral("PPSA01317"), Qt::CaseInsensitive) == 0;
}

// The release launcher is desktop-only. Legacy VR keys remain readable for settings compatibility,
// but cannot turn into VR command-line arguments through any launcher entry point.
inline void ApplyGt7DesktopPreset(Configuration& info) {
	if (!IsGt7Title(info)) {
		return;
	}
	info.gt7_vr_enabled = false;
	info.printf_direction = Configuration::LogDirection::Silent;
	if (!info.gt7_dlss_enabled) {
		return;
	}
	// Keep the persisted switch compatible with 1.0.3, but select this release's
	// verified 1440p -> 4K reconstruction profile. It no longer enables FG.
	info.gt7_desktop_height = 0;
	info.screen_resolution = Configuration::Resolution::R3840X2160;
	info.present_mode = Configuration::PresentMode::Mailbox;
	info.fullscreen_enabled = true;
	// This is the title's normal virtual refresh rate, not an extra host FPS cap.
	info.vblank_frequency = 60;
	info.tessellation_enabled = true;
	info.bindless_images_enabled = true;
	info.reflections_half_rate = false;
	info.skip_shader_hashes.clear();
	info.readback_linear_images = false;
	info.shader_optimization_type = Configuration::ShaderOptimizationType::Performance;
	info.shader_log_direction = Configuration::LogDirection::Silent;
	info.vulkan_validation_enabled = false;
	info.shader_validation_enabled = false;
	info.command_buffer_dump_enabled = false;
	info.profiler_enabled = false;
	info.renderdoc_enabled = false;
#if defined(_WIN32)
	info.red_zone_protection_enabled = true;
#endif
}

// These are profile requests, not authorization to patch an executable. The emulator verifies
// the exact executable fingerprint and instruction bytes before applying its in-memory patch.
inline bool HasGt7Profile(const Configuration& info) {
	return IsGt7Title(info) && (info.gt7_dlss_enabled || info.gt7_desktop_height != 0);
}

inline QStringList Gt7ProfileArguments(const Configuration& info) {
	if (!HasGt7Profile(info)) {
		return {};
	}
	if (info.gt7_dlss_enabled) {
		return {QStringLiteral("--gt7-desktop-height"), QStringLiteral("native"),
		        QStringLiteral("--ps5-pro"), QStringLiteral("--pssr-dlss"),
		        QStringLiteral("--gt7-pssr-1440-4k")};
	}
	return {QStringLiteral("--gt7-desktop-height"), QString::number(info.gt7_desktop_height)};
}

// The release preset uses only SR. Optional frame-generation libraries must
// neither be loaded nor become a prerequisite for this launch path.
inline QStringList Gt7GraphicsRuntimeFiles() {
	return {QStringLiteral("kyty_dlss_backend.dll"), QStringLiteral("nvngx_dlss.dll")};
}

inline QStringList MissingGt7GraphicsRuntimeFiles(const QString& runtime_directory) {
	QStringList missing;
	const QDir runtime(runtime_directory);
	for (const auto& name: Gt7GraphicsRuntimeFiles()) {
		if (!QFileInfo(runtime.absoluteFilePath(name)).isFile()) missing << name;
	}
	return missing;
}

inline QProcessEnvironment Gt7LaunchEnvironment(const Configuration& info,
                                                const QString& runtime_directory,
                                                QProcessEnvironment environment) {
	if (!IsGt7Title(info)) {
		return environment;
	}
	// Playtest/diagnostic shells must not leak capture or tuning overrides into ordinary play.
	for (const auto& key: environment.keys()) {
		if (key.startsWith(QStringLiteral("KYTY_"), Qt::CaseInsensitive) ||
		    key.startsWith(QStringLiteral("GT7_"), Qt::CaseInsensitive)) {
			environment.remove(key);
		}
	}
	environment.insert(QStringLiteral("KYTY_FG_MANUAL_ONLY"), QStringLiteral("1"));
	// Neither switch position may inherit the old TAA hook, either FG provider,
	// the 30 FPS experiment, or asynchronous presentation from a testing shell.
	environment.insert(QStringLiteral("KYTY_GT7_DLSS"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_GT7_FSR_FG"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_GT7_FSR_FG_OCCLUSION_GUARD"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_GT7_DLSS_FG"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_GT7_DLSS_FG_BASE_FPS"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_PSSR_DIAGNOSTICS"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_GT7_NATIVE_ASYNC_PRESENT"), QStringLiteral("0"));
	environment.insert(QStringLiteral("KYTY_SAMPLE"), QStringLiteral("0"));
	if (info.gt7_dlss_enabled) {
		const QDir runtime(runtime_directory);
		environment.insert(QStringLiteral("KYTY_GT7_DLSS_BACKEND"),
		                   runtime.absoluteFilePath(QStringLiteral("kyty_dlss_backend.dll")));
		environment.insert(QStringLiteral("KYTY_GT7_DLSS_RUNTIME"), runtime.absolutePath());
		environment.insert(QStringLiteral("KYTY_GT7_DLSS_PRESET"), QStringLiteral("M"));
		// Per-process opt-out, matching the tested Vulkan profile.
		environment.insert(QStringLiteral("DISABLE_RTSS_LAYER"), QStringLiteral("1"));
	}
	return environment;
}

#endif
