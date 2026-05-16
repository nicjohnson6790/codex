#pragma once

class LineRenderer;
class QuadtreeMeshRenderer;
class TriangleRenderer;
class WorldTextRenderer;

struct RenderEngines
{
    TriangleRenderer& triangleRenderer;
    LineRenderer& lineRenderer;
    QuadtreeMeshRenderer* quadtreeMeshRenderer = nullptr;
    WorldTextRenderer* worldTextRenderer = nullptr;
};
