//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

// PERFORMANCE TIP: Set max recursion depth as low as needed
// as drivers may apply optimization strategies for low recursion depths.
#define MAX_RAY_RECURSION_DEPTH 3    // ~ primary rays + reflections + shadow rays from reflected geometry.

struct ProceduralPrimitiveAttributes
{
    vec3 normal;
};

struct RayPayload
{
    vec4 color;
    UINT recursionDepth;
};

struct ShadowRayPayload
{
    bool hit;
};

struct SceneConstantBuffer
{
    mat4 projectionToWorld;
    vec4 cameraPosition;
    vec4 lightPosition;
    vec4 lightAmbientColor;
    vec4 lightDiffuseColor;
    float reflectance;
    float elapsedTime;                 // Elapsed application time.
};

// Attributes per primitive type.
struct PrimitiveConstantBuffer
{
    vec4 albedo;
    float reflectanceCoef;
    float diffuseCoef;
    float specularCoef;
    float specularPower;
    float stepScale;                      // Step scale for ray marching of signed distance primitives. 
    // - Some object transformations don't preserve the distances and 
    //   thus require shorter steps.
    vec3 padding;
};

// Attributes per primitive instance.
struct PrimitiveInstanceConstantBuffer
{
    UINT instanceIndex;
    UINT primitiveType; // Procedural primitive type
};

// Dynamic attributes per primitive instance.
struct PrimitiveInstancePerFrameBuffer
{
    mat4 localSpaceToBottomLevelAS;   // Matrix from local primitive space to bottom-level object space.
    mat4 bottomLevelASToLocalSpace;   // Matrix from bottom-level object space to local primitive space.
};

struct Vertex
{
    vec3 position;
    vec3 normal;
};

// Ray types traced in this sample.
enum RAY_TYPE {
    RAY_TYPE_RADIANCE, // ~ Primary, reflected camera/view rays calculating color for each hit.
    RAY_TYPE_SHADOW,   // ~ Shadow/visibility rays, only testing for occlusion
    RAY_TYPE_COUNT
};

// From: http://blog.selfshadow.com/publications/s2015-shading-course/hoffman/s2015_pbs_physics_math_slides.pdf
static vec4 ChromiumReflectance = { 0.549f, 0.556f, 0.554f, 1.0f };

enum ANALYTIC_PRIMITIVE
{
    ANALYTIC_PRIMITIVE_AABB,
    ANALYTIC_PRIMITIVE_SPHERES,
    ANALYTIC_PRIMITIVE_COUNT
};

enum VOLUMETRIC_PRIMITIVE_TYPE
{
    VOLUMETRIC_PRIMITIVE_METABALLS,
    VOLUMETRIC_PRIMITIVE_COUNT
};

enum SIGNED_DISTANCE_PRIMITIVE
{
    SIGNED_DISTANCE_PRIMITIVE_MINISPHERES,
    SIGNED_DISTANCE_PRIMITIVE_INTERSECTED_ROUND_CUBE,
    SIGNED_DISTANCE_PRIMITIVE_SQUARE_TORUS,
    SIGNED_DISTANCE_PRIMITIVE_TWISTED_TORUS,
    SIGNED_DISTANCE_PRIMITIVE_COG,
    SIGNED_DISTANCE_PRIMITIVE_CYLINDER,
    SIGNED_DISTANCE_PRIMITIVE_FRACTAL_PYRAMID,
    SIGNED_DISTANCE_PRIMITIVE_COUNT
};
