//
// Description : Reflected water radiance
//
// Ported from tuxalin/water-shader (shaders/hlsl/water/radiance.cginc).
// Z-up: the light's elevation is lightDir.z (lightDir.y in the original).
// BLINN_PHONG is a runtime flag here (material "blinnPhong").
//

#ifndef WATER_RADIANCE_HLSLI
#define WATER_RADIANCE_HLSLI

// refractionValues, x = index of refraction constant, y = refraction strength
// normal and eyeVec in world space
float FresnelValue(float2 refractionValues, float3 normal, float3 eyeVec)
{
    // R0 is a constant related to the index of refraction (IOR).
    float R0 = refractionValues.x;
    // This value modifies current fresnel term. If you want to weaken
    // reflections use bigger value.
    float refractionStrength = refractionValues.y;

    float angle = 1.0f - saturate(dot(normal, eyeVec));
    float fresnel = angle * angle;
    fresnel *= fresnel;
    fresnel *= angle;
    return saturate(fresnel * (1.0f - saturate(R0)) + R0 - refractionStrength);
}

// lightDir, eyeDir and normal in world space
float3 ReflectedRadiance(float shininess, float3 specularValues, float3 lightColor, float3 lightDir, float3 eyeDir,
    float3 normal, float fresnel, bool blinnPhong)
{
    float shininessExp = specularValues.z;
    float3 specular;

    if (blinnPhong)
    {
        // a variant of the blinn phong shading
        float specularIntensity = specularValues.x * 0.0075;

        float3 H = normalize(eyeDir + lightDir);
        float e = shininess * shininessExp * 800;
        float kS = saturate(dot(normal, lightDir));
        specular = kS * specularIntensity * pow(saturate(dot(normal, H)), e) * sqrt((e + 1) / 2);
        specular *= lightColor;
    }
    else
    {
        float2 specularIntensity = specularValues.xy;
        // reflect the eye vector such that the incident and emergent angles are equal
        float3 mirrorEye = reflect(-eyeDir, normal);
        float dotSpec = saturate(dot(mirrorEye, lightDir) * 0.5f + 0.5f);
        specular = (1.0f - fresnel) * saturate(lightDir.z) * pow(dotSpec, specularIntensity.y) * (shininess * shininessExp + 0.2f) * lightColor;
        specular += specular * specularIntensity.x * saturate(shininess - 0.05f) * lightColor;
    }
    return specular;
}

#endif // WATER_RADIANCE_HLSLI
