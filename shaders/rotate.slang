[[vk::push_constant]]
cbuffer ComputePC
{
    uint64_t dst_bda;  // offset 0, 8 bytes
    float    angle;    // offset 8, 4 bytes
};

struct RotationBuf { float4x4 model; };

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    float c = cos(angle);
    float s = sin(angle);

    // Y-axis rotation
    float4x4 ry = float4x4(
        float4( c,   0.0, s,   0.0),
        float4( 0.0, 1.0, 0.0, 0.0),
        float4(-s,   0.0, c,   0.0),
        float4( 0.0, 0.0, 0.0, 1.0)
    );

    // Rx(90°) — keeps the DamagedHelmet upright
    float4x4 rx = float4x4(
        float4(1.0, 0.0,  0.0, 0.0),
        float4(0.0, 0.0, -1.0, 0.0),
        float4(0.0, 1.0,  0.0, 0.0),
        float4(0.0, 0.0,  0.0, 1.0)
    );

    RotationBuf* dst = (RotationBuf*)dst_bda;
    dst->model = mul(ry, rx);
}
