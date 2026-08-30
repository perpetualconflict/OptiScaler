#include "pch.h"

#include "RrProfile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <json.hpp>

namespace RayReconstruction
{
namespace
{
std::string Lower(std::string value)
{
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

NormalSpace ParseNormalSpace(const std::string& value)
{
    if (value == "world")
        return NormalSpace::World;
    if (value == "view")
        return NormalSpace::View;
    return NormalSpace::Unspecified;
}

MatrixConversion ParseMatrixConversion(const std::string& value)
{
    if (value == "direct_copy")
        return MatrixConversion::DirectCopy;
    if (value == "transpose")
        return MatrixConversion::Transpose;
    return MatrixConversion::Unspecified;
}

DepthConvention ParseDepthConvention(const std::string& value)
{
    if (value == "hardware")
        return DepthConvention::Hardware;
    if (value == "linear_signed")
        return DepthConvention::LinearSigned;
    return DepthConvention::Unspecified;
}

MotionVectorDirection ParseMotionDirection(const std::string& value)
{
    if (value == "previous_minus_current")
        return MotionVectorDirection::PreviousMinusCurrent;
    if (value == "current_minus_previous")
        return MotionVectorDirection::CurrentMinusPrevious;
    return MotionVectorDirection::Unspecified;
}

DepthDeltaSource ParseDepthDeltaSource(const std::string& value)
{
    if (value == "reprojected_history")
        return DepthDeltaSource::ReprojectedHistory;
    if (value == "camera_reprojection")
        return DepthDeltaSource::CameraReprojection;
    return DepthDeltaSource::Unspecified;
}

SignalAdapter ParseSignalAdapter(const std::string& value)
{
    if (value == "composite_floor_split")
        return SignalAdapter::CompositeFloorSplit;
    if (value == "composite_albedo_split")
        return SignalAdapter::CompositeAlbedoSplit;
    if (value == "direct_resources")
        return SignalAdapter::DirectResources;
    return SignalAdapter::Disabled;
}

std::optional<FfxRr12::Signal> ParseSignal(const std::string& value)
{
    if (value == "ambient_occlusion")
        return FfxRr12::Signal::AmbientOcclusion;
    if (value == "direct_diffuse")
        return FfxRr12::Signal::DirectDiffuse;
    if (value == "direct_specular")
        return FfxRr12::Signal::DirectSpecular;
    if (value == "dominant_light_visibility")
        return FfxRr12::Signal::DominantLightVisibility;
    if (value == "indirect_diffuse")
        return FfxRr12::Signal::IndirectDiffuse;
    if (value == "indirect_specular")
        return FfxRr12::Signal::IndirectSpecular;
    if (value == "specular_occlusion")
        return FfxRr12::Signal::SpecularOcclusion;
    return std::nullopt;
}

FfxRr12::SignalMask ParseSignalMask(const nlohmann::json& values)
{
    FfxRr12::SignalMask result = 0;
    for (const auto& value : values)
    {
        const auto signal = ParseSignal(Lower(value.get<std::string>()));
        if (!signal)
            throw std::runtime_error("unknown FSR-RR signal '" + value.get<std::string>() + "'");
        result |= FfxRr12::ToMask(*signal);
    }
    return result;
}
} // namespace

bool Profile::IsDispatchable(std::string& reason) const
{
    if (!enabled)
        reason = "profile is disabled";
    else if (!validated)
        reason = "profile has not been capture-validated";
    else if (normalSpace == NormalSpace::Unspecified)
        reason = "normal space is unspecified";
    else if (matrixConversion == MatrixConversion::Unspecified)
        reason = "matrix conversion is unspecified";
    else if (depthConvention == DepthConvention::Unspecified)
        reason = "depth convention is unspecified";
    else if (motionVectorDirection == MotionVectorDirection::Unspecified)
        reason = "motion-vector direction is unspecified";
    else if (depthDeltaSource == DepthDeltaSource::Unspecified)
        reason = "depth-delta source is unspecified";
    else if (signalAdapter == SignalAdapter::Disabled || signals == 0)
        reason = "no signal adapter or signal mask is enabled";
    else if ((checkerboardSignals & ~signals) != 0)
        reason = "checkerboard signal mask is not a subset of active signals";
    else if (!(linearDepthMin >= 0.0f && linearDepthMin < linearDepthMax))
        reason = "linear depth bounds are invalid";
    else
    {
        reason.clear();
        return true;
    }

    return false;
}

bool ProfileDatabase::Load(const std::filesystem::path& path)
{
    _profiles.clear();
    _lastError.clear();

    try
    {
        std::ifstream stream(path);
        if (!stream)
            throw std::runtime_error("unable to open " + path.string());

        const auto root = nlohmann::json::parse(stream);
        if (root.value("schema_version", 0) != 1)
            throw std::runtime_error("unsupported or missing profile schema_version");

        for (const auto& value : root.at("profiles"))
        {
            Profile profile;
            profile.id = value.at("id").get<std::string>();
            profile.executables = value.at("executables").get<std::vector<std::string>>();
            profile.enabled = value.value("enabled", false);
            profile.validated = value.value("validated", false);
            profile.normalSpace = ParseNormalSpace(Lower(value.value("normal_space", "unspecified")));
            profile.matrixConversion =
                ParseMatrixConversion(Lower(value.value("matrix_conversion", "unspecified")));
            profile.depthConvention = ParseDepthConvention(Lower(value.value("depth_convention", "unspecified")));
            profile.motionVectorDirection =
                ParseMotionDirection(Lower(value.value("motion_vector_direction", "unspecified")));
            profile.depthDeltaSource =
                ParseDepthDeltaSource(Lower(value.value("depth_delta_source", "unspecified")));
            profile.signalAdapter = ParseSignalAdapter(Lower(value.value("signal_adapter", "disabled")));
            profile.signals = ParseSignalMask(value.value("signals", nlohmann::json::array()));
            profile.checkerboardSignals =
                ParseSignalMask(value.value("checkerboard_signals", nlohmann::json::array()));
            profile.linearDepthMin = value.value("linear_depth_min", 0.0f);
            profile.linearDepthMax = value.value("linear_depth_max", 0.0f);
            profile.notes = value.value("notes", "");
            _profiles.push_back(std::move(profile));
        }
    }
    catch (const std::exception& exception)
    {
        _lastError = exception.what();
        LOG_ERROR("FSR-RR profiles: {}", _lastError);
        _profiles.clear();
        return false;
    }

    LOG_INFO("FSR-RR profiles: loaded {} profile(s) from {}", _profiles.size(), path.string());
    return true;
}

const Profile* ProfileDatabase::FindForExecutable(std::string executable) const
{
    executable = Lower(std::filesystem::path(executable).filename().string());
    for (const auto& profile : _profiles)
        for (const auto& candidate : profile.executables)
            if (Lower(std::filesystem::path(candidate).filename().string()) == executable)
                return &profile;
    return nullptr;
}
} // namespace RayReconstruction
