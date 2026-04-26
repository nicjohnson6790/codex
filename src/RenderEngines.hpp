#pragma once

class LineRenderer;
class QuadtreeMeshRenderer;
class TriangleRenderer;

struct RenderEngines
{
    TriangleRenderer& triangleRenderer;
    LineRenderer& lineRenderer;
    QuadtreeMeshRenderer* quadtreeMeshRenderer = nullptr;
};
