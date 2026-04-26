#include "CameraManager.hpp"

#include "AppConfig.hpp"

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <stdexcept>
#include <utility>

std::size_t CameraManager::createCamera(
    std::string name,
    const Position& position,
    const glm::dvec3& forward,
    const glm::dvec3& up
)
{
    m_cameras.push_back({
        .name = std::move(name),
        .position = position,
        .forward = glm::normalize(forward),
        .up = glm::normalize(up),
    });
    return m_cameras.size() - 1;
}

const CameraManager::Camera& CameraManager::activeCamera() const
{
    if (m_cameras.empty())
    {
        throw std::runtime_error("No active camera is available.");
    }

    return m_cameras[m_activeCameraIndex];
}

CameraManager::Camera& CameraManager::activeCamera()
{
    if (m_cameras.empty())
    {
        throw std::runtime_error("No active camera is available.");
    }

    return m_cameras[m_activeCameraIndex];
}

const Position& CameraManager::activeCameraPosition() const
{
    return activeCamera().position;
}

void CameraManager::setActiveCamera(std::size_t index)
{
    if (index < m_cameras.size())
    {
        m_activeCameraIndex = index;
    }
}

glm::mat4 CameraManager::buildActiveViewProjectionMatrix(Extent2D viewportExtent) const
{
    const Camera& camera = activeCamera();

    glm::dvec3 forward = glm::normalize(camera.forward);
    glm::dvec3 right = glm::normalize(glm::cross(forward, camera.up));
    if (glm::length(right) <= 0.00001)
    {
        right = { 1.0, 0.0, 0.0 };
    }
    const glm::dvec3 up = glm::normalize(glm::cross(right, forward));

    const float aspectRatio =
        static_cast<float>(glm::max(viewportExtent.width, 1u)) /
        static_cast<float>(glm::max(viewportExtent.height, 1u));

    const glm::mat4 view = glm::lookAtRH(
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(static_cast<float>(forward.x), static_cast<float>(forward.y), static_cast<float>(forward.z)),
        glm::vec3(static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z))
    );
    const float tanHalfFov = std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5f);
    glm::mat4 projection(0.0f);
    projection[0][0] = 1.0f / (aspectRatio * tanHalfFov);
    projection[1][1] = 1.0f / tanHalfFov;
    projection[2][3] = -1.0f;
    projection[3][2] = AppConfig::Camera::kNearPlane;
    return projection * view;
}
