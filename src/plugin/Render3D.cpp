// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Render3D.h"

#include <d3dcompiler.h>

namespace Render3D
{
	namespace
	{
		// ---------------------------------------------------------------- shaders
		// Lighting as the preview's three.js scene: hemisphere fill (sky ffd9a8, ground 1a120a, 0.42), a warm key light from
		// up and to the left (ffcf94, 1.35) with soft shadows, and a candle off to the right (ff8a3a, range 45), ACES filmic
		// tone mapping at exposure 1.1. Textures are the game's own (sRGB content in UNORM formats), normal maps carry the
		// specular mask in alpha, as Skyrim's do.
		constexpr const char* kShader = R"(
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;
	row_major float4x4 lightViewProj;
	float4 camPos;
	float4 keyDir;      // toward the light
	float4 keyColor;
	float4 sky;
	float4 ground;
	float4 candlePos;
	float4 candleColor; // w: range
	float4 params;      // x time, y shadow texel, z exposure
};
cbuffer Object : register(b1)
{
	float4 rot0, rot1, rot2;
	float4 posScale;
	float4 spec;        // rgb * scale, w gloss
	float4 misc;        // x env scale, y alpha, z alpha ref (0..1), w alpha test
	uint4  has;         // diffuse, normal, env, env mask
	float4 pbr;         // x roughness (>= 0: a die), y metalness
	uint4  has2;        // x glow
};

Texture2D    texDiffuse : register(t0);
Texture2D    texNormal  : register(t1);
TextureCube  texEnv     : register(t2);
Texture2D    texEnvMask : register(t3);
Texture2D    texShadow  : register(t4);
Texture2D    texGlow    : register(t5);
SamplerState smpWrap    : register(s0);
SamplerComparisonState smpShadow : register(s1);

struct VSIn
{
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
	float4 nrm : NORMAL;
	float4 tan : TANGENT;
};
struct VSOut
{
	float4 svpos : SV_Position;
	float3 wpos  : TEXCOORD0;
	float2 uv    : TEXCOORD1;
	float3 n     : TEXCOORD2;
	float3 t     : TEXCOORD3;
	float3 b     : TEXCOORD4;
};

float3 ToWorldDir(float3 v) { return float3(dot(rot0.xyz, v), dot(rot1.xyz, v), dot(rot2.xyz, v)); }
float3 ToWorld(float3 p) { return ToWorldDir(p) * posScale.w + posScale.xyz; }

VSOut VSMain(VSIn i)
{
	VSOut o;
	float3 wp = ToWorld(i.pos.xyz);
	o.svpos = mul(viewProj, float4(wp, 1));
	o.wpos = wp;
	o.uv = i.uv;
	o.n = ToWorldDir(i.nrm.xyz * 2 - 1);
	o.t = ToWorldDir(i.tan.xyz * 2 - 1);
	o.b = ToWorldDir(float3(i.pos.w, i.nrm.w * 2 - 1, i.tan.w * 2 - 1));
	return o;
}

float4 VSShadow(VSIn i) : SV_Position
{
	return mul(lightViewProj, float4(ToWorld(i.pos.xyz), 1));
}

float Shadow(float3 wp, float3 n)
{
	float4 lp = mul(lightViewProj, float4(wp + n * 0.06, 1));
	float2 uv = float2(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
	if (any(uv < 0) || any(uv > 1) || lp.z > 1) return 1;
	float s = 0, d = lp.z - 0.0015;
	[unroll] for (int y = -2; y <= 2; y++)
		[unroll] for (int x = -2; x <= 2; x++)
			s += texShadow.SampleCmpLevelZero(smpShadow, uv + float2(x, y) * params.y * 1.2, d);
	return s / 25;
}

float3 Lin(float3 c) { return pow(saturate(c), 2.2); }


// ---- the dice, lit exactly as the preview's three.js MeshStandardMaterial (r128): GGX, a metal mask, and the preview's
// warm room as the environment (a gradient with three lamp-glows, blurred by roughness as PMREM does)
static const float PI = 3.14159265;
float3 EnvGrad(float t)  // the canvas gradient, top (0) to bottom (1), in sRGB
{
	const float3 c0 = float3(0x3a, 0x2c, 0x20) / 255, c1 = float3(0x6b, 0x50, 0x38) / 255, c2 = float3(0x4a, 0x36, 0x24) / 255, c3 = float3(0x2a, 0x1e, 0x14) / 255;
	if (t < 0.45) return lerp(c0, c1, t / 0.45);
	if (t < 0.55) return lerp(c1, c2, (t - 0.45) / 0.1);
	return lerp(c2, c3, (t - 0.55) / 0.45);
}
float3 EnvRad(float3 d, float rough)
{
	// table space (z up) to three's (y up): (x, z, -y); equirect u, v as three maps them, in the 256 x 128 canvas
	float3 t3 = float3(d.x, d.z, -d.y);
	float u = atan2(t3.z, t3.x) / (2 * PI) + 0.5;
	float v = 1 - (asin(clamp(t3.y, -1, 1)) / PI + 0.5);  // 0 top
	float2 px = float2(u * 256, v * 128);
	float blur = rough * rough;
	float3 g = EnvGrad(saturate(v));
	g = lerp(g, float3(0x4a, 0x37, 0x27) / 255, smoothstep(0, 0.8, blur));
	const float3 spots[3] = { float3(60, 30, 22), float3(190, 40, 16), float3(130, 22, 10) };
	float3 col = g;
	[unroll] for (int k = 0; k < 3; k++) {
		float2 dd = px - spots[k].xy;
		dd.x = dd.x - 256 * round(dd.x / 256);
		float r0 = spots[k].z, r = sqrt(r0 * r0 + (blur * 70) * (blur * 70));
		float a = saturate(1 - length(dd) / r) * (r0 * r0) / (r * r);
		float3 sc = lerp(float3(255, 220, 160), float3(255, 200, 120), saturate(length(dd) / r)) / 255;
		col = lerp(col, sc, a);
	}
	return Lin(col);
}
float D_GGX(float a, float nh) { float a2 = a * a; float d = nh * nh * (a2 - 1) + 1; return a2 / (PI * d * d); }
float V_Smith(float a, float nl, float nv)
{
	float a2 = a * a;
	float gv = nl * sqrt(a2 + (1 - a2) * nv * nv), gl = nv * sqrt(a2 + (1 - a2) * nl * nl);
	return 0.5 / max(gv + gl, 1e-6);
}
float3 F_Schlick(float3 f0, float vh) { float f = exp2((-5.55473 * vh - 6.98316) * vh); return f0 * (1 - f) + f; }
float3 DirectSpec(float3 f0, float rough, float3 N, float3 V, float3 L)
{
	float3 H = normalize(L + V);
	float nl = saturate(dot(N, L)), nv = saturate(dot(N, V)), nh = saturate(dot(N, H)), vh = saturate(dot(V, H));
	float a = rough * rough;
	return F_Schlick(f0, vh) * V_Smith(a, nl, nv) * D_GGX(a, nh);
}
float3 DFG(float3 f0, float rough, float nv)
{
	const float4 c0 = float4(-1, -0.0275, -0.572, 0.022), c1 = float4(1, 0.0425, 1.04, -0.04);
	float4 r = rough * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
	float2 fab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * fab.x + fab.y;
}
float4 ShadeDie(VSOut i, float4 dif, float3 N, float3 V)
{
	float3 albedo = Lin(dif.rgb);
	float  m = pbr.y * (has.w ? texEnvMask.Sample(smpWrap, i.uv).r : 1);
	float3 dN = abs(ddx(N)) + abs(ddy(N));
	float  rough = min(max(pbr.x, 0.0525) + max(max(dN.x, dN.y), dN.z), 1);
	float3 diff = albedo * (1 - m);
	float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, m);
	float  nv = saturate(dot(N, V));
	float3 L = keyDir.xyz;
	float  nl = saturate(dot(N, L));
	float  sh = Shadow(i.wpos, N);
	float3 toC = candlePos.xyz - i.wpos;
	float  dc = length(toC);
	float3 Lc = toC / max(dc, 1e-3);
	float  att = pow(saturate(1 - dc / candleColor.w), 2);
	float  flick = 0.88 + 0.07 * sin(params.x * 13.1) + 0.05 * sin(params.x * 7.3 + 1.7);
	float3 candle = candleColor.rgb * att * flick;
	float  nlc = saturate(dot(N, Lc));
	// three (not physically-correct lights): punctual irradiance is colour * NdotL * PI, the diffuse BRDF albedo / PI
	float3 direct = keyColor.rgb * nl * sh * (diff + PI * DirectSpec(f0, rough, N, V, L)) + candle * nlc * (diff + PI * DirectSpec(f0, rough, N, V, Lc));
	float3 hemi = lerp(ground.rgb, sky.rgb, N.z * 0.5 + 0.5);
	float3 envIrr = EnvRad(N, 1);
	float3 R = reflect(-V, N);
	float3 indirect = diff * (hemi + envIrr) + EnvRad(R, rough) * DFG(f0, rough, nv);
	float3 glow = has2.x ? Lin(texGlow.Sample(smpWrap, i.uv).rgb) : float3(0, 0, 0);
	return float4(direct + indirect + glow, 1);
}

float4 PSMain(VSOut i) : SV_Target
{
	float4 dif = has.x ? texDiffuse.Sample(smpWrap, i.uv) : float4(0.6, 0.6, 0.6, 1);
	float alpha = dif.a * misc.y;
	if (misc.w > 0.5) clip(alpha - misc.z);
	float3 albedo = Lin(dif.rgb);
	float3 N = normalize(i.n);
	float  mask = 1;
	if (has.y) {
		float4 nm = texNormal.Sample(smpWrap, i.uv);
		float3 t = normalize(i.t - N * dot(i.t, N));
		float3 b = normalize(i.b - N * dot(i.b, N));
		float3 d = nm.xyz * 2 - 1;
		N = normalize(d.x * t + d.y * b + d.z * N);
		mask = nm.a;
	}
	float3 V = normalize(camPos.xyz - i.wpos);
	if (dot(N, V) < -0.2) N = -N;  // an inside face seen from inside (the cups)
	if (pbr.x >= 0) return ShadeDie(i, dif, N, V);
	float3 L = keyDir.xyz;
	float  nl = saturate(dot(N, L));
	float  sh = Shadow(i.wpos, N);
	float3 hemi = lerp(ground.rgb, sky.rgb, N.z * 0.5 + 0.5);
	float3 toC = candlePos.xyz - i.wpos;
	float  dc = length(toC);
	float3 Lc = toC / max(dc, 1e-3);
	float  att = pow(saturate(1 - dc / candleColor.w), 2);
	float  flick = 0.88 + 0.07 * sin(params.x * 13.1) + 0.05 * sin(params.x * 7.3 + 1.7);
	float3 candle = candleColor.rgb * att * flick;
	float  nlc = saturate(dot(N, Lc));
	float3 light = hemi + keyColor.rgb * nl * sh + candle * nlc;
	float3 H = normalize(L + V), Hc = normalize(Lc + V);
	float  g = max(spec.w, 1);
	float  norm = (g + 8) / 25.13;
	float3 sp = spec.rgb * mask * norm * (pow(saturate(dot(N, H)), g) * nl * sh * keyColor.rgb + pow(saturate(dot(N, Hc)), g) * nlc * candle);
	float3 env = 0;
	if (has.z) {
		float3 R = reflect(-V, N);
		float  m = has.w ? texEnvMask.Sample(smpWrap, i.uv).r : mask;
		float  fres = 0.35 + 0.65 * pow(1 - saturate(dot(N, V)), 4);
		env = Lin(texEnv.SampleLevel(smpWrap, R, 2).rgb) * misc.x * m * fres * (hemi * 2.2 + keyColor.rgb * 0.25);
	}
	float3 col = albedo * light + sp + env;
	return float4(col * alpha, alpha);
}

// ---- full-screen passes
struct FSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FSOut VSFull(uint id : SV_VertexID)
{
	FSOut o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
SamplerState smpClamp : register(s2);
cbuffer PassCB : register(b2) { float4 passArgs; };  // xy texel, z amount, w direction/strength

float4 PSDown(FSOut i) : SV_Target
{
	float2 t = passArgs.xy;
	float3 c = texDiffuse.Sample(smpClamp, i.uv + t * float2(-1, -1)).rgb + texDiffuse.Sample(smpClamp, i.uv + t * float2(1, -1)).rgb +
	           texDiffuse.Sample(smpClamp, i.uv + t * float2(-1, 1)).rgb + texDiffuse.Sample(smpClamp, i.uv + t * float2(1, 1)).rgb;
	return float4(c * 0.25, 1);
}
float4 PSBlur(FSOut i) : SV_Target
{
	const float w[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
	float2 d = passArgs.w > 0.5 ? float2(0, passArgs.y) : float2(passArgs.x, 0);
	float3 c = texDiffuse.Sample(smpClamp, i.uv).rgb * w[0];
	[unroll] for (int k = 1; k < 5; k++)
		c += (texDiffuse.Sample(smpClamp, i.uv + d * k * 1.6).rgb + texDiffuse.Sample(smpClamp, i.uv - d * k * 1.6).rgb) * w[k];
	return float4(c, 1);
}
float4 PSBack(FSOut i) : SV_Target
{
	// the world behind, blurred, darker and a little desaturated (a paused Skyrim menu)
	float3 c = texDiffuse.Sample(smpClamp, i.uv).rgb;
	float  l = dot(c, float3(0.299, 0.587, 0.114));
	c = lerp(float3(l, l, l), c, 0.72) * 0.62;
	return float4(c, passArgs.z);
}
float3 ACES(float3 c)
{
	const float3x3 inM = { 0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777 };
	const float3x3 outM = { 1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602 };
	c = mul(c, inM);
	float3 a = c * (c + 0.0245786) - 0.000090537;
	float3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
	c = mul(a / b, outM);
	return saturate(c);
}
// The Shout: a cone of air bends the whole frame, fringed with colour and lit faintly blue at the wavefront (as the
// preview's post pass). passArgs: x time 0..1, y aspect.
float4 PSShout(FSOut i) : SV_Target
{
	const float T = passArgs.x, aspect = passArgs.y;
	float2 vUv = float2(i.uv.x, 1 - i.uv.y);
	float2 d = vUv - float2(0.5, -0.32);
	d.x *= aspect;
	float r = length(d);
	float2 dir = d / max(r, 1e-4);
	float ang = abs(atan2(d.x, d.y));
	float cone = 1.0 - smoothstep(0.42, 0.9, ang);
	float x = r - T * 2.0;
	float band = exp(-x * x / 0.005) * cone;
	float wake = (1.0 - smoothstep(-0.5, 0.0, x)) * smoothstep(-1.3, -0.35, x) * cone;
	float fade = 1.0 - smoothstep(0.5, 1.0, T);
	float ripple = sin(x * 75.0 - T * 45.0);
	float amt = (band * 0.04 + wake * 0.007 * ripple) * fade;
	float2 off = dir * amt;
	off.x /= aspect;
	off.y = -off.y;  // back to texture space (v down)
	float3 col;
	col.r = texDiffuse.Sample(smpClamp, i.uv - off).r;
	col.g = texDiffuse.Sample(smpClamp, i.uv - off * 1.1).g;
	col.b = texDiffuse.Sample(smpClamp, i.uv - off * 1.22).b;
	col += float3(0.55, 0.72, 1.0) * band * 0.22 * fade;
	col = lerp(col, col * float3(0.92, 0.97, 1.08) + 0.02, wake * fade * 0.6);
	return float4(col, 1);
}
float4 PSComposite(FSOut i) : SV_Target
{
	float4 c = texDiffuse.Sample(smpClamp, i.uv);
	if (c.a <= 0.0001) return 0;
	float3 rgb = c.rgb / c.a;
	rgb = ACES(rgb * passArgs.w / 0.6);
	rgb = pow(rgb, 1 / 2.2);
	float a = c.a * passArgs.z;
	return float4(rgb * a, a);
}
)";

		struct CBFrame
		{
			float viewProj[16];
			float lightViewProj[16];
			float camPos[4];
			float keyDir[4];
			float keyColor[4];
			float sky[4];
			float ground[4];
			float candlePos[4];
			float candleColor[4];
			float params[4];
		};
		struct CBObject
		{
			float         rot0[4], rot1[4], rot2[4];
			float         posScale[4];
			float         spec[4];
			float         misc[4];
			std::uint32_t has[4];
			float         pbr[4];
			std::uint32_t has2[4];
		};

		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		std::mutex  g_mtx;
		Frame       g_frame;
		bool        g_failed = false, g_ready = false;
		ID3D11Device* g_dev = nullptr;

		ID3D11VertexShader* g_vs = nullptr;
		ID3D11VertexShader* g_vsShadow = nullptr;
		ID3D11VertexShader* g_vsFull = nullptr;
		ID3D11PixelShader*  g_ps = nullptr;
		ID3D11PixelShader*  g_psDown = nullptr;
		ID3D11PixelShader*  g_psBlur = nullptr;
		ID3D11PixelShader*  g_psBack = nullptr;
		ID3D11PixelShader*  g_psComposite = nullptr;
		ID3D11PixelShader*  g_psShout = nullptr;
		std::vector<char>   g_vsCode;
		ID3D11Buffer*       g_cbFrame = nullptr;
		ID3D11Buffer*       g_cbObject = nullptr;
		ID3D11Buffer*       g_cbPass = nullptr;
		ID3D11Buffer*       g_dummyVB = nullptr;
		ID3D11SamplerState* g_smpWrap = nullptr;
		ID3D11SamplerState* g_smpShadow = nullptr;
		ID3D11SamplerState* g_smpClamp = nullptr;
		ID3D11RasterizerState*   g_rsNoCull = nullptr;
		ID3D11RasterizerState*   g_rsShadow = nullptr;
		ID3D11DepthStencilState* g_dsOn = nullptr;
		ID3D11DepthStencilState* g_dsRead = nullptr;
		ID3D11DepthStencilState* g_dsOff = nullptr;
		ID3D11BlendState*        g_bsOpaque = nullptr;
		ID3D11BlendState*        g_bsPremul = nullptr;
		ID3D11BlendState*        g_bsAlpha = nullptr;
		std::unordered_map<std::uint64_t, ID3D11InputLayout*> g_layouts;

		// size-dependent targets
		UINT                      g_w = 0, g_h = 0, g_msaa = 1;
		DXGI_FORMAT               g_backFmt = DXGI_FORMAT_UNKNOWN;
		ID3D11Texture2D*          g_color = nullptr;  // MSAA, linear HDR, premultiplied
		ID3D11RenderTargetView*   g_colorRTV = nullptr;
		ID3D11Texture2D*          g_depth = nullptr;
		ID3D11DepthStencilView*   g_depthDSV = nullptr;
		ID3D11Texture2D*          g_resolved = nullptr;
		ID3D11ShaderResourceView* g_resolvedSRV = nullptr;
		ID3D11Texture2D*          g_bg = nullptr;  // a copy of the frame
		ID3D11ShaderResourceView* g_bgSRV = nullptr;
		ID3D11Texture2D*          g_q[2]{};  // quarter size, for the blur
		ID3D11RenderTargetView*   g_qRTV[2]{};
		ID3D11ShaderResourceView* g_qSRV[2]{};
		constexpr UINT            kShadow = 2048;
		ID3D11Texture2D*          g_shadow = nullptr;
		ID3D11DepthStencilView*   g_shadowDSV = nullptr;
		ID3D11ShaderResourceView* g_shadowSRV = nullptr;

		ID3DBlob* Compile(const char* a_entry, const char* a_target)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* err = nullptr;
			const HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "SkyrimYacht", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
			if (FAILED(hr)) {
				spdlog::error("Table renderer: shader {} failed: {}", a_entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
				Release(err);
				Release(code);
				return nullptr;
			}
			Release(err);
			return code;
		}

		bool Init(ID3D11Device* a_dev)
		{
			g_dev = a_dev;
			auto vs = [&](const char* e, ID3D11VertexShader** out, bool keep) {
				auto* b = Compile(e, "vs_5_0");
				if (!b) {
					return false;
				}
				const bool ok = SUCCEEDED(a_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
				if (keep) {
					g_vsCode.assign(static_cast<const char*>(b->GetBufferPointer()), static_cast<const char*>(b->GetBufferPointer()) + b->GetBufferSize());
				}
				b->Release();
				return ok;
			};
			auto ps = [&](const char* e, ID3D11PixelShader** out) {
				auto* b = Compile(e, "ps_5_0");
				if (!b) {
					return false;
				}
				const bool ok = SUCCEEDED(a_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
				b->Release();
				return ok;
			};
			if (!vs("VSMain", &g_vs, true) || !vs("VSShadow", &g_vsShadow, false) || !vs("VSFull", &g_vsFull, false) || !ps("PSMain", &g_ps) ||
				!ps("PSDown", &g_psDown) || !ps("PSBlur", &g_psBlur) || !ps("PSBack", &g_psBack) || !ps("PSComposite", &g_psComposite) || !ps("PSShout", &g_psShout)) {
				return false;
			}
			auto cb = [&](UINT size, ID3D11Buffer** out) {
				D3D11_BUFFER_DESC d{};
				d.ByteWidth = (size + 15) & ~15u;
				d.Usage = D3D11_USAGE_DYNAMIC;
				d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				return SUCCEEDED(a_dev->CreateBuffer(&d, nullptr, out));
			};
			if (!cb(sizeof(CBFrame), &g_cbFrame) || !cb(sizeof(CBObject), &g_cbObject) || !cb(16, &g_cbPass)) {
				return false;
			}
			// stand-ins for attributes a mesh doesn't have: no UV, a normal facing up, a tangent along x
			{
				const std::uint8_t dummy[16] = { 0, 0, 0, 0, 128, 128, 255, 128, 255, 128, 128, 128, 0, 0, 0, 0 };
				D3D11_BUFFER_DESC  d{};
				d.ByteWidth = sizeof(dummy);
				d.Usage = D3D11_USAGE_IMMUTABLE;
				d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				D3D11_SUBRESOURCE_DATA s{ dummy, 0, 0 };
				if (FAILED(a_dev->CreateBuffer(&d, &s, &g_dummyVB))) {
					return false;
				}
			}
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_ANISOTROPIC;
			sd.MaxAnisotropy = 8;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			a_dev->CreateSamplerState(&sd, &g_smpWrap);
			sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
			sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
			sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
			sd.MaxAnisotropy = 1;
			a_dev->CreateSamplerState(&sd, &g_smpShadow);
			sd = {};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			a_dev->CreateSamplerState(&sd, &g_smpClamp);
			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			rd.MultisampleEnable = TRUE;
			rd.AntialiasedLineEnable = FALSE;
			a_dev->CreateRasterizerState(&rd, &g_rsNoCull);
			rd.MultisampleEnable = FALSE;
			rd.DepthBias = 300;
			rd.SlopeScaledDepthBias = 2.0f;
			a_dev->CreateRasterizerState(&rd, &g_rsShadow);
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			a_dev->CreateDepthStencilState(&dd, &g_dsOn);
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			a_dev->CreateDepthStencilState(&dd, &g_dsRead);
			dd.DepthEnable = FALSE;
			a_dev->CreateDepthStencilState(&dd, &g_dsOff);
			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			a_dev->CreateBlendState(&bd, &g_bsOpaque);
			auto& rt = bd.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ONE;
			rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			a_dev->CreateBlendState(&bd, &g_bsPremul);
			rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			a_dev->CreateBlendState(&bd, &g_bsAlpha);
			// the shadow map
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = kShadow;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32_TYPELESS;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &g_shadow))) {
				return false;
			}
			D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
			dsv.Format = DXGI_FORMAT_D32_FLOAT;
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			a_dev->CreateDepthStencilView(g_shadow, &dsv, &g_shadowDSV);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_FLOAT;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			a_dev->CreateShaderResourceView(g_shadow, &srv, &g_shadowSRV);
			spdlog::info("Table renderer ready");
			return g_shadowDSV && g_shadowSRV;
		}

		void FreeTargets()
		{
			Release(g_colorRTV);
			Release(g_color);
			Release(g_depthDSV);
			Release(g_depth);
			Release(g_resolvedSRV);
			Release(g_resolved);
			Release(g_bgSRV);
			Release(g_bg);
			for (int k = 0; k < 2; ++k) {
				Release(g_qRTV[k]);
				Release(g_qSRV[k]);
				Release(g_q[k]);
			}
			g_w = g_h = 0;
		}

		DXGI_FORMAT Typed(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return f;
			}
		}

		bool Targets(const D3D11_TEXTURE2D_DESC& a_back)
		{
			if (g_w == a_back.Width && g_h == a_back.Height && g_backFmt == a_back.Format && g_color) {
				return true;
			}
			FreeTargets();
			g_w = a_back.Width;
			g_h = a_back.Height;
			g_backFmt = a_back.Format;
			UINT q = 0;
			g_msaa = 4;
			if (FAILED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, 4, &q)) || q == 0) {
				g_msaa = 1;
			}
			D3D11_TEXTURE2D_DESC td{};
			td.Width = g_w;
			td.Height = g_h;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			td.SampleDesc.Count = g_msaa;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_color)) && SUCCEEDED(g_dev->CreateRenderTargetView(g_color, nullptr, &g_colorRTV));
			td.Format = DXGI_FORMAT_D32_FLOAT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_depth)) && SUCCEEDED(g_dev->CreateDepthStencilView(g_depth, nullptr, &g_depthDSV));
			td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_resolved)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_resolved, nullptr, &g_resolvedSRV));
			// the frame's copy (same format as the back buffer, so it can be copied), for the blur
			D3D11_TEXTURE2D_DESC bd = a_back;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bd.MiscFlags = 0;
			bd.CPUAccessFlags = 0;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.SampleDesc.Count = 1;
			bd.SampleDesc.Quality = 0;
			bd.MipLevels = 1;
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = Typed(a_back.Format);
			sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D.MipLevels = 1;
			ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&bd, nullptr, &g_bg)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_bg, &sv, &g_bgSRV));
			td.Width = (std::max)(1u, g_w / 4);
			td.Height = (std::max)(1u, g_h / 4);
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			for (int k = 0; k < 2; ++k) {
				ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_q[k])) && SUCCEEDED(g_dev->CreateRenderTargetView(g_q[k], nullptr, &g_qRTV[k])) &&
				     SUCCEEDED(g_dev->CreateShaderResourceView(g_q[k], nullptr, &g_qSRV[k]));
			}
			if (!ok) {
				spdlog::error("Table renderer: couldn't make its {}x{} targets (back buffer format {})", g_w, g_h, static_cast<int>(a_back.Format));
				FreeTargets();
				return false;
			}
			spdlog::info("Table renderer: {}x{}, {}x MSAA, back buffer format {}", g_w, g_h, g_msaa, static_cast<int>(a_back.Format));
			return true;
		}

		ID3D11InputLayout* Layout(std::uint64_t a_desc)
		{
			const std::uint64_t key = a_desc;
			if (auto it = g_layouts.find(key); it != g_layouts.end()) {
				return it->second;
			}
			const auto flags = static_cast<std::uint16_t>(a_desc >> 44);
			auto       off = [&](int attr) { return static_cast<UINT>((a_desc >> (4 * attr + 2)) & 0x3C); };
			// full precision positions take 16 bytes (the flag isn't always set on the renderer's copy): see where the next
			// attribute starts
			UINT next = static_cast<UINT>(a_desc & 0xF) * 4;
			for (int attr : { 1, 3, 4, 5 }) {
				if (flags & (1 << attr)) {
					next = (std::min)(next, off(attr));
				}
			}
			const bool full = (flags & 0x400) != 0 || next >= 16;
			D3D11_INPUT_ELEMENT_DESC e[4]{};
			e[0] = { "POSITION", 0, full ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, off(0), D3D11_INPUT_PER_VERTEX_DATA, 0 };
			e[1] = (flags & (1 << 1)) ? D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, off(1), D3D11_INPUT_PER_VERTEX_DATA, 0 }
			                          : D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			e[2] = (flags & (1 << 3)) ? D3D11_INPUT_ELEMENT_DESC{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, off(3), D3D11_INPUT_PER_VERTEX_DATA, 0 }
			                          : D3D11_INPUT_ELEMENT_DESC{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			e[3] = (flags & (1 << 4)) ? D3D11_INPUT_ELEMENT_DESC{ "TANGENT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, off(4), D3D11_INPUT_PER_VERTEX_DATA, 0 }
			                          : D3D11_INPUT_ELEMENT_DESC{ "TANGENT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			ID3D11InputLayout* il = nullptr;
			if (FAILED(g_dev->CreateInputLayout(e, 4, g_vsCode.data(), g_vsCode.size(), &il))) {
				spdlog::warn("Table renderer: no input layout for vertex format {:016X}", a_desc);
			}
			g_layouts[key] = il;
			return il;
		}

		template <class T>
		void Upload(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_buf, const T& a_data)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(a_ctx->Map(a_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, &a_data, sizeof(T));
				a_ctx->Unmap(a_buf, 0);
			}
		}

		// The game's own state, put back exactly afterwards (as Dear ImGui's DX11 backend does)
		struct Saved
		{
			UINT                      vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_VIEWPORT            vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_RECT                sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			ID3D11RasterizerState*    rs = nullptr;
			ID3D11BlendState*         bs = nullptr;
			FLOAT                     bf[4]{};
			UINT                      mask = 0;
			ID3D11DepthStencilState*  ds = nullptr;
			UINT                      ref = 0;
			ID3D11ShaderResourceView* srv[5]{};
			ID3D11SamplerState*       smp[3]{};
			ID3D11PixelShader*        ps = nullptr;
			ID3D11VertexShader*       vs = nullptr;
			ID3D11GeometryShader*     gs = nullptr;
			ID3D11Buffer*             vcb[3]{};
			ID3D11Buffer*             pcb[3]{};
			D3D11_PRIMITIVE_TOPOLOGY  topo{};
			ID3D11Buffer*             ib = nullptr;
			DXGI_FORMAT               ibFmt{};
			UINT                      ibOff = 0;
			ID3D11Buffer*             vb[2]{};
			UINT                      stride[2]{}, offset[2]{};
			ID3D11InputLayout*        il = nullptr;
			ID3D11RenderTargetView*   rtv = nullptr;
			ID3D11DepthStencilView*   dsv = nullptr;

			void Take(ID3D11DeviceContext* c)
			{
				c->RSGetViewports(&vpCount, vp);
				c->RSGetScissorRects(&scCount, sc);
				c->RSGetState(&rs);
				c->OMGetBlendState(&bs, bf, &mask);
				c->OMGetDepthStencilState(&ds, &ref);
				c->PSGetShaderResources(0, 5, srv);
				c->PSGetSamplers(0, 3, smp);
				c->PSGetShader(&ps, nullptr, nullptr);
				c->VSGetShader(&vs, nullptr, nullptr);
				c->GSGetShader(&gs, nullptr, nullptr);
				c->VSGetConstantBuffers(0, 3, vcb);
				c->PSGetConstantBuffers(0, 3, pcb);
				c->IAGetPrimitiveTopology(&topo);
				c->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
				c->IAGetVertexBuffers(0, 2, vb, stride, offset);
				c->IAGetInputLayout(&il);
				c->OMGetRenderTargets(1, &rtv, &dsv);
			}
			void Put(ID3D11DeviceContext* c)
			{
				c->RSSetViewports(vpCount, vp);
				c->RSSetScissorRects(scCount, sc);
				c->RSSetState(rs);
				c->OMSetBlendState(bs, bf, mask);
				c->OMSetDepthStencilState(ds, ref);
				c->PSSetShaderResources(0, 5, srv);
				c->PSSetSamplers(0, 3, smp);
				c->PSSetShader(ps, nullptr, 0);
				c->VSSetShader(vs, nullptr, 0);
				c->GSSetShader(gs, nullptr, 0);
				c->VSSetConstantBuffers(0, 3, vcb);
				c->PSSetConstantBuffers(0, 3, pcb);
				c->IASetPrimitiveTopology(topo);
				c->IASetIndexBuffer(ib, ibFmt, ibOff);
				c->IASetVertexBuffers(0, 2, vb, stride, offset);
				c->IASetInputLayout(il);
				c->OMSetRenderTargets(1, &rtv, dsv);
				Release(rs);
				Release(bs);
				Release(ds);
				for (auto*& s : srv) {
					Release(s);
				}
				for (auto*& s : smp) {
					Release(s);
				}
				Release(ps);
				Release(vs);
				Release(gs);
				for (auto*& b : vcb) {
					Release(b);
				}
				for (auto*& b : pcb) {
					Release(b);
				}
				Release(ib);
				for (auto*& b : vb) {
					Release(b);
				}
				Release(il);
				Release(rtv);
				Release(dsv);
			}
		};

		void FullScreen(ID3D11DeviceContext* c, ID3D11RenderTargetView* a_rtv, UINT w, UINT h, ID3D11PixelShader* a_ps, ID3D11ShaderResourceView* a_src,
			const float (&a_pass)[4], ID3D11BlendState* a_bs)
		{
			D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1 };
			c->RSSetViewports(1, &vp);
			c->OMSetRenderTargets(1, &a_rtv, nullptr);
			c->OMSetBlendState(a_bs, nullptr, 0xFFFFFFFF);
			Upload(c, g_cbPass, a_pass);
			c->PSSetConstantBuffers(2, 1, &g_cbPass);
			c->PSSetShader(a_ps, nullptr, 0);
			c->PSSetShaderResources(0, 1, &a_src);
			c->Draw(3, 0);
			ID3D11ShaderResourceView* none = nullptr;
			c->PSSetShaderResources(0, 1, &none);
		}

		// 4x4 helpers (row-major, clip = M * v)
		using M4 = std::array<float, 16>;
		M4 Mul(const M4& a, const M4& b)
		{
			M4 r{};
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					float s = 0;
					for (int k = 0; k < 4; ++k) {
						s += a[i * 4 + k] * b[k * 4 + j];
					}
					r[i * 4 + j] = s;
				}
			}
			return r;
		}
		M4 LookAt(const float e[3], const float t[3], const float up[3])
		{
			float f[3] = { t[0] - e[0], t[1] - e[1], t[2] - e[2] };
			const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
			for (auto& v : f) {
				v /= fl;
			}
			float r[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
			const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
			for (auto& v : r) {
				v /= rl;
			}
			const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
			auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
			return { r[0], r[1], r[2], -dot(r, e), u[0], u[1], u[2], -dot(u, e), -f[0], -f[1], -f[2], dot(f, e), 0, 0, 0, 1 };
		}
		M4 Ortho(float l, float r, float b, float t, float n, float f)
		{
			return { 2 / (r - l), 0, 0, -(r + l) / (r - l), 0, 2 / (t - b), 0, -(t + b) / (t - b), 0, 0, 1 / (n - f), n / (n - f), 0, 0, 0, 1 };
		}

		std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
	}

	void Submit(Frame&& a_frame)
	{
		std::lock_guard lk(g_mtx);
		g_frame = std::move(a_frame);
	}

	void Clear()
	{
		std::lock_guard lk(g_mtx);
		g_frame = Frame{};
	}

	void Draw(IDXGISwapChain* a_sc, ID3D11Device* a_dev, ID3D11DeviceContext* c)
	{
		std::lock_guard lk(g_mtx);
		if (!g_frame.show || g_failed || !a_sc || !a_dev || !c) {
			return;
		}
		if (!g_ready) {
			g_ready = Init(a_dev);
			if (!g_ready) {
				g_failed = true;
				spdlog::error("Table renderer: couldn't start - the table won't show");
				return;
			}
		}
		ID3D11Texture2D* back = nullptr;
		if (FAILED(a_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
			return;
		}
		D3D11_TEXTURE2D_DESC bdsc{};
		back->GetDesc(&bdsc);
		if (!Targets(bdsc)) {
			back->Release();
			g_failed = true;
			return;
		}
		ID3D11RenderTargetView*        backRTV = nullptr;
		D3D11_RENDER_TARGET_VIEW_DESC  rvd{};
		rvd.Format = Typed(bdsc.Format);
		rvd.ViewDimension = bdsc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		a_dev->CreateRenderTargetView(back, &rvd, &backRTV);
		if (!backRTV) {
			back->Release();
			return;
		}
		Saved saved;
		saved.Take(c);

		const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_t0).count();
		c->VSSetShader(g_vsFull, nullptr, 0);
		c->GSSetShader(nullptr, nullptr, 0);
		c->IASetInputLayout(nullptr);
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c->RSSetState(g_rsNoCull);
		c->OMSetDepthStencilState(g_dsOff, 0);
		ID3D11SamplerState* smp[3] = { g_smpWrap, g_smpShadow, g_smpClamp };
		c->PSSetSamplers(0, 3, smp);

		// 1. the world behind: blurred and darkened, as Skyrim's menus do
		if (g_frame.blur > 0.001f && bdsc.SampleDesc.Count == 1) {
			c->CopyResource(g_bg, back);
			const UINT qw = (std::max)(1u, g_w / 4), qh = (std::max)(1u, g_h / 4);
			const float down[4] = { 1.0f / g_w, 1.0f / g_h, 0, 0 };
			FullScreen(c, g_qRTV[0], qw, qh, g_psDown, g_bgSRV, down, g_bsOpaque);
			for (int k = 0; k < 2; ++k) {
				const float h[4] = { 1.0f / qw, 1.0f / qh, 0, 0 }, v[4] = { 1.0f / qw, 1.0f / qh, 0, 1 };
				FullScreen(c, g_qRTV[1], qw, qh, g_psBlur, g_qSRV[0], h, g_bsOpaque);
				FullScreen(c, g_qRTV[0], qw, qh, g_psBlur, g_qSRV[1], v, g_bsOpaque);
			}
			const float b[4] = { 0, 0, g_frame.blur, 0 };
			FullScreen(c, backRTV, g_w, g_h, g_psBack, g_qSRV[0], b, g_bsAlpha);
		}

		// 2. the frame's constants: our camera, the key light's shadow camera, the lights
		CBFrame fr{};
		std::memcpy(fr.viewProj, g_frame.viewProj, sizeof(fr.viewProj));
		{
			const float lp[3] = { -10, -14, 30 }, at[3] = { 0, 0, 0 }, up[3] = { 0, 0, 1 };
			const M4    lv = LookAt(lp, at, up);
			const M4    lo = Ortho(-20, 20, -18, 18, 5, 70);
			const M4    lvp = Mul(lo, lv);
			std::memcpy(fr.lightViewProj, lvp.data(), sizeof(fr.lightViewProj));
			const float len = std::sqrt(10.0f * 10 + 14 * 14 + 30 * 30);
			fr.keyDir[0] = lp[0] / len;
			fr.keyDir[1] = lp[1] / len;
			fr.keyDir[2] = lp[2] / len;
		}
		auto lin = [](int hex, float k, float* out) {
			for (int i = 0; i < 3; ++i) {
				const float v = ((hex >> (16 - 8 * i)) & 0xFF) / 255.0f;
				out[i] = std::pow(v, 2.2f) * k;
			}
		};
		lin(0xffcf94, 1.35f, fr.keyColor);
		lin(0xffd9a8, 0.42f, fr.sky);
		lin(0x1a120a, 0.42f, fr.ground);
		fr.candlePos[0] = 17;
		fr.candlePos[1] = 3;
		fr.candlePos[2] = 9;
		lin(0xff8a3a, 1.0f, fr.candleColor);
		fr.candleColor[3] = 45;
		std::memcpy(fr.camPos, g_frame.camPos, sizeof(float) * 3);
		fr.params[0] = time;
		fr.params[1] = 1.0f / kShadow;
		fr.params[2] = 1.1f;
		Upload(c, g_cbFrame, fr);
		ID3D11Buffer* cbs[2] = { g_cbFrame, g_cbObject };
		c->VSSetConstantBuffers(0, 2, cbs);
		c->PSSetConstantBuffers(0, 2, cbs);

		// once per table: what each mesh's buffers really hold, against what its layout says
		static bool s_checked = false;
		if (!s_checked && !g_frame.items.empty()) {
			s_checked = true;
			for (const auto& it : g_frame.items) {
				D3D11_BUFFER_DESC vd{}, id{};
				if (it.vb) {
					it.vb->GetDesc(&vd);
				}
				if (it.ib) {
					it.ib->GetDesc(&id);
				}
				const UINT stride = static_cast<UINT>(it.desc & 0xF) * 4;
				spdlog::info("  mesh '{}': layout {:016X} (geometry says {:016X}), flags {:04X}, stride {}, {} verts -> {} bytes (buffer {}), {} indices (buffer {}), pos at {}, uv at {}, normal at {}, tangent at {}, diffuse {}, normal map {}",
					it.name, it.desc, it.geoDesc, static_cast<unsigned>(it.desc >> 44), stride, it.vertexCount, it.vertexCount * stride, vd.ByteWidth,
					it.indexCount, id.ByteWidth / 2, (it.desc >> 2) & 0x3C, (it.desc >> 6) & 0x3C, (it.desc >> 14) & 0x3C, (it.desc >> 18) & 0x3C,
					it.diffuse != nullptr, it.normal != nullptr);
			}
		}

		auto bindMesh = [&](const Item& it) -> bool {
			auto* il = Layout(it.desc);
			if (!il || !it.vb || !it.ib) {
				return false;
			}
			const UINT    stride = static_cast<UINT>(it.desc & 0xF) * 4;
			ID3D11Buffer* vbs[2] = { it.vb, g_dummyVB };
			UINT          strides[2] = { stride, 0 }, offs[2] = { 0, 0 };
			c->IASetInputLayout(il);
			c->IASetVertexBuffers(0, 2, vbs, strides, offs);
			c->IASetIndexBuffer(it.ib, DXGI_FORMAT_R16_UINT, 0);
			CBObject ob{};
			for (int k = 0; k < 3; ++k) {
				ob.rot0[k] = it.rot[k];
				ob.rot1[k] = it.rot[3 + k];
				ob.rot2[k] = it.rot[6 + k];
				ob.posScale[k] = it.pos[k];
				ob.spec[k] = it.specColor[k] * it.specScale;
			}
			ob.posScale[3] = it.scale;
			ob.spec[3] = it.gloss;
			ob.misc[0] = it.envScale;
			ob.misc[1] = it.alpha;
			ob.misc[2] = it.alphaRef;
			ob.misc[3] = it.test ? 1.0f : 0.0f;
			ob.has[0] = it.diffuse != nullptr;
			ob.has[1] = it.normal != nullptr;
			ob.has[2] = it.env != nullptr;
			ob.has[3] = it.envMask != nullptr;
			ob.pbr[0] = it.rough;
			ob.pbr[1] = it.metal;
			ob.has2[0] = it.glow != nullptr;
			Upload(c, g_cbObject, ob);
			return true;
		};

		// 3. the key light's shadows
		{
			c->OMSetRenderTargets(0, nullptr, g_shadowDSV);
			c->ClearDepthStencilView(g_shadowDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
			D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kShadow), static_cast<float>(kShadow), 0, 1 };
			c->RSSetViewports(1, &vp);
			c->RSSetState(g_rsShadow);
			c->OMSetDepthStencilState(g_dsOn, 0);
			c->VSSetShader(g_vsShadow, nullptr, 0);
			c->PSSetShader(nullptr, nullptr, 0);
			for (const auto& it : g_frame.items) {
				if (!it.blend && bindMesh(it)) {
					c->DrawIndexed(it.indexCount, 0, 0);
				}
			}
		}

		// 4. the table, lit, into our multisampled HDR target
		{
			const float clear[4] = { 0, 0, 0, 0 };
			c->ClearRenderTargetView(g_colorRTV, clear);
			c->ClearDepthStencilView(g_depthDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
			c->OMSetRenderTargets(1, &g_colorRTV, g_depthDSV);
			D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1 };
			c->RSSetViewports(1, &vp);
			c->RSSetState(g_rsNoCull);
			c->VSSetShader(g_vs, nullptr, 0);
			c->PSSetShader(g_ps, nullptr, 0);
			c->PSSetShaderResources(4, 1, &g_shadowSRV);
			auto drawPass = [&](bool a_blend) {
				c->OMSetBlendState(a_blend ? g_bsPremul : g_bsOpaque, nullptr, 0xFFFFFFFF);
				c->OMSetDepthStencilState(a_blend ? g_dsRead : g_dsOn, 0);
				for (const auto& it : g_frame.items) {
					if (it.blend != a_blend || !bindMesh(it)) {
						continue;
					}
					ID3D11ShaderResourceView* t[4] = { it.diffuse, it.normal, it.env, it.envMask };
					c->PSSetShaderResources(0, 4, t);
					c->PSSetShaderResources(5, 1, &it.glow);
					c->DrawIndexed(it.indexCount, 0, 0);
				}
			};
			drawPass(false);
			drawPass(true);
			ID3D11ShaderResourceView* none[6]{};
			c->PSSetShaderResources(0, 6, none);
			ID3D11RenderTargetView* noRT = nullptr;
			c->OMSetRenderTargets(1, &noRT, nullptr);
			if (g_msaa > 1) {
				c->ResolveSubresource(g_resolved, 0, g_color, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
			} else {
				c->CopyResource(g_resolved, g_color);
			}
		}

		// 5. onto the screen: tone mapped, faded in as it arrives
		{
			c->IASetInputLayout(nullptr);
			c->VSSetShader(g_vsFull, nullptr, 0);
			c->RSSetState(g_rsNoCull);
			c->OMSetDepthStencilState(g_dsOff, 0);
			const float p[4] = { 0, 0, g_frame.reveal, 1.1f };
			FullScreen(c, backRTV, g_w, g_h, g_psComposite, g_resolvedSRV, p, g_bsPremul);
		}

		// 6. the Shout bends the finished frame
		if (g_frame.shout >= 0.0f && bdsc.SampleDesc.Count == 1) {
			c->OMSetRenderTargets(0, nullptr, nullptr);
			c->CopyResource(g_bg, back);
			const float p[4] = { (std::min)(1.0f, g_frame.shout), g_frame.aspect, 0, 0 };
			FullScreen(c, backRTV, g_w, g_h, g_psShout, g_bgSRV, p, g_bsOpaque);
		}

		saved.Put(c);
		backRTV->Release();
		back->Release();
	}
}
