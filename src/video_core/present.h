// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/settings.h"

static inline Settings::ScalingFilter GetScalingFilter() {
    // The AMD SDK's temporal path runs inside the game's DLSS replacement.
    // Never stack FSR 1 or another spatial presentation effect on top of it.
    // Bilinear handles window sizing, menus and unsupported content safely.
    if (Settings::values.official_amd_fsr31.GetValue()) {
        return Settings::ScalingFilter::Bilinear;
    }
    return Settings::values.scaling_filter.GetValue();
}

static inline Settings::AntiAliasing GetAntiAliasing() {
    return Settings::values.anti_aliasing.GetValue();
}

static inline Settings::ScalingFilter GetScalingFilterForAppletCapture() {
    return Settings::ScalingFilter::Bilinear;
}

static inline Settings::AntiAliasing GetAntiAliasingForAppletCapture() {
    return Settings::AntiAliasing::None;
}

struct PresentFilters {
    Settings::ScalingFilter (*get_scaling_filter)();
    Settings::AntiAliasing (*get_anti_aliasing)();
};

constexpr PresentFilters PresentFiltersForDisplay{
    .get_scaling_filter = &GetScalingFilter,
    .get_anti_aliasing = &GetAntiAliasing,
};

constexpr PresentFilters PresentFiltersForAppletCapture{
    .get_scaling_filter = &GetScalingFilterForAppletCapture,
    .get_anti_aliasing = &GetAntiAliasingForAppletCapture,
};
