#pragma once
// Shader HLSL embebido (se compila en runtime con D3DCompile).
// Si prefieres evitar D3DCompiler_47.dll, precompila con: fxc /T ps_5_0 /E PSMain /Fh ...

static const char* kShaderSrc = R"HLSL(

Texture2D<float4> gCurrent : register(t0);   // copia del fotograma actual (backbuffer)
Texture2D<float4> gHistory : register(t1);   // salida acumulada del fotograma anterior

cbuffer Params : register(b0)
{
    float gDt;          // tiempo real entre Presents (segundos)
    float gSlider;      // 0 = sin blur ... 1 = blur máximo (control deslizante)
    float gShutter;     // fracción de exposición: 1.0 = 360°, 0.5 = 180° (cine)
    float gMaxTrail;    // tope de la constante de tiempo en modo fluido (segundos)
    float gMinFps;      // FPS simulados con slider = 1  (p.ej. 12)
    float gMaxFps;      // FPS simulados con slider = 0  (p.ej. 500)
    float gGammaSpace;  // 1 = backbuffer UNORM no-sRGB -> mezclar en luz lineal
    float gReset;       // 1 = primer fotograma / cambio de resolución -> ignorar historial
    float gIntensity;   // 0..2 multiplicador de la cantidad de blur (1 = normal)
    float gOsdValue;    // 0..1 relleno de la barra en pantalla
    float gOsdAlpha;    // 0..1 visibilidad de la barra (0 = oculta)
    float gOsdKind;     // 0 = FPS simulados, 1 = intensidad
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// Triángulo a pantalla completa sin vertex buffer
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = uv;
    return o;
}

// ---------------------------------------------------------------------------
//  NÚCLEO MATEMÁTICO
// ---------------------------------------------------------------------------

// Mapa logarítmico slider -> FPS simulados. Logarítmico porque la percepción
// del movimiento es proporcional (240->120 se nota igual que 60->30).
//   slider 0.0 -> gMaxFps (500)      slider ~0.2 -> 240 FPS
//   slider ~0.57 -> 60 FPS            slider ~0.81 -> 24 FPS (cine)
float SliderToSimFps(float s)
{
    return exp(lerp(log(gMaxFps), log(gMinFps), saturate(s)));
}

// Peso del fotograma ACTUAL en la mezcla (alpha). Fotograma final:
//      out = lerp(history, current, alpha)
//
// Es un promedio exponencial independiente del framerate real:
//      alpha = 1 - exp(-dt / tau)
// donde tau es la "constante de tiempo" del obturador simulado.
//
//  - Ventana de exposición:  T = shutter / simFps
//  - Un EMA con constante tau tiene edad media = tau; una ventana rectangular
//    de longitud T tiene edad media T/2  ->  tau = T / 2
//  - Tope tau <= trailCap: evita estelas "borrachas" (más generoso en modo cine)
//
// Como depende de dt (no del nº de frames), el resultado se ve igual a 60, 144
// o 250 FPS reales. Limitación física: si el FPS real es MENOR que el simulado,
// alpha -> 1 y el efecto desaparece (no se pueden inventar fotogramas).
float ComputeBlendFactor(float dt, float slider, out float simFps, out float cinematic)
{
    simFps = SliderToSimFps(slider);

    // 0 = modo fluido (>= ~90 FPS simulados), 1 = modo cinematográfico (<= ~40)
    cinematic = smoothstep(90.0, 40.0, simFps);

    float exposure = gShutter / simFps;
    float tau      = 0.5 * exposure;

    float trailCap = lerp(gMaxTrail, gMaxTrail * 4.0, cinematic);
    tau = clamp(tau, 1e-5, trailCap);

    // INTENSIDAD: multiplica la duración de la estela.
    //   0 = sin blur, 1 = normal, 2 = el doble de estela
    if (gIntensity <= 0.001) return 1.0;
    tau = max(tau * gIntensity, 1e-6);

    return saturate(1.0 - exp(-dt / tau));
}

float3 ToWork(float3 c)   { return gGammaSpace > 0.5 ? pow(max(c, 0.0), 2.2)       : c; }
float3 FromWork(float3 c) { return gGammaSpace > 0.5 ? pow(max(c, 0.0), 1.0 / 2.2) : c; }

// Barra en pantalla (esquina superior izquierda). Se dibuja SOLO en la salida visible,
// no en el historial, para que no deje estela.
float4 DrawOsd(int2 p, float4 col, float height)
{
    if (gOsdAlpha <= 0.0) return col;

    float s = max(1.0, height / 1080.0);
    float2 origin = float2(24.0, 24.0) * s;
    float2 size   = float2(240.0, 10.0) * s;
    float2 q      = float2(p) - origin;
    float  b      = 2.0 * s;                     // borde

    if (q.x < -b || q.y < -b || q.x > size.x + b || q.y > size.y + b) return col;

    bool inside = q.x >= 0.0 && q.y >= 0.0 && q.x <= size.x && q.y <= size.y;
    float3 barCol = gOsdKind > 0.5 ? float3(1.0, 0.75, 0.2) : float3(0.3, 0.8, 1.0);

    float3 c = float3(0.0, 0.0, 0.0);            // borde
    if (inside)
    {
        float u = q.x / size.x;
        c = (u <= gOsdValue) ? barCol : float3(0.15, 0.15, 0.15);
        // marca blanca en el 100% de la intensidad (centro de la barra)
        if (gOsdKind > 0.5 && abs(u - 0.5) < 0.006) c = float3(1.0, 1.0, 1.0);
    }

    col.rgb = lerp(col.rgb, c, 0.9 * gOsdAlpha);
    return col;
}

struct PSOut
{
    float4 screen  : SV_Target0;   // -> backbuffer (lo que se ve)
    float4 history : SV_Target1;   // -> textura de historial (siguiente frame)
};

PSOut PSMain(VSOut i)
{
    int2 p = int2(i.pos.xy);
    uint w, h;
    gCurrent.GetDimensions(w, h);

    float4 curRaw = gCurrent.Load(int3(p, 0));
    float3 cur    = ToWork(curRaw.rgb);

    float simFps, cinematic;
    float alpha = ComputeBlendFactor(gDt, gSlider, simFps, cinematic);

    // Primer fotograma o historial inválido -> sin mezcla
    if (gReset > 0.5) alpha = 1.0;

    float3 result = cur;

    if (alpha < 0.999)
    {
        float3 hist = ToWork(gHistory.Load(int3(p, 0)).rgb);

        // --- Rechazo de ghosting: acotar el historial a la vecindad 3x3 actual ---
        // (con tolerancia: estricta en modo fluido, amplia en modo cine)
        float3 mn = cur, mx = cur;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(p + int2(x, y), int2(0, 0), int2(w - 1, h - 1));
            float3 n = ToWork(gCurrent.Load(int3(q, 0)).rgb);
            mn = min(mn, n);
            mx = max(mx, n);
        }
        float tol = lerp(0.03, 0.35, cinematic) * max(1.0, gIntensity);
        hist = clamp(hist, mn - tol, mx + tol);

        result = lerp(hist, cur, alpha);
    }

    float4 outCol = float4(FromWork(result), curRaw.a);

    PSOut o;
    o.screen  = DrawOsd(p, outCol, (float)h);
    o.history = outCol;
    return o;
}

)HLSL";
