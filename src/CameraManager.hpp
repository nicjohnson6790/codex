#pragma once

#include "Position.hpp"
#include "RenderTypes.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <string>
#include <vector>

class CameraManager
{
public:
    struct Camera
    {
        std::string name;
        Position position;
        glm::dvec3 forward{ 0.0, 0.0, -1.0 };
        glm::dvec3 up{ 0.0, 1.0, 0.0 };
    };

    std::size_t createCamera(
        std::string name,
        const Position& position,
        const glm::dvec3& forward = { 0.0, 0.0, -1.0 },
        const glm::dvec3& up = { 0.0, 1.0, 0.0 }
    );

    [[nodiscard]] std::size_t cameraCount() const { return m_cameras.size(); }
    [[nodiscard]] bool hasActiveCamera() const { return !m_cameras.empty(); }
    [[nodiscard]] std::size_t activeCameraIndex() const { return m_activeCameraIndex; }
    [[nodiscard]] const Camera& activeCamera() const;
    [[nodiscard]] Camera& activeCamera();
    [[nodiscard]] const Position& activeCameraPosition() const;

    void setActiveCamera(std::size_t index);

    [[nodiscard]] glm::mat4 buildActiveViewProjectionMatrix(Extent2D viewportExtent) const;

private:
    std::vector<Camera> m_cameras;
    std::size_t m_activeCameraIndex = 0;
};
