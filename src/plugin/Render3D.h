// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

struct ID3D11Buffer;
struct ID3D11ShaderResourceView;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

// The table drawn by us, over the game, as the browser preview draws it: our own camera and lights (a warm key light
// with soft shadows, a hemisphere fill, a flickering candle), MSAA, filmic tone mapping, and the world behind blurred.
// The meshes are the game's own (loaded with BSModelDB): we draw their vertex and index buffers and textures directly.
namespace Render3D
{
	struct Item
	{
		ID3D11Buffer*             vb = nullptr;
		ID3D11Buffer*             ib = nullptr;
		std::uint64_t             desc = 0;  // BSGraphics::VertexDesc
		std::uint32_t             indexCount = 0;
		float                     rot[9]{};  // world rotation (row-major), translation, uniform scale
		float                     pos[3]{};
		float                     scale = 1.0f;
		ID3D11ShaderResourceView* diffuse = nullptr;
		ID3D11ShaderResourceView* normal = nullptr;
		ID3D11ShaderResourceView* env = nullptr;  // a cube map
		ID3D11ShaderResourceView* envMask = nullptr;
		float                     specColor[3]{ 1, 1, 1 };
		float                     specScale = 1.0f, gloss = 30.0f, envScale = 0.0f, alpha = 1.0f, alphaRef = 0.0f;
		bool                      blend = false;
		bool                      test = false;
	};

	struct Frame
	{
		std::vector<Item> items;
		float             viewProj[16]{};  // row-major: clip = M * (x, y, z, 1), in table space (z up)
		float             camPos[3]{};
		float             reveal = 0.0f;  // the table fading in (0..1)
		float             blur = 0.0f;    // the world behind blurred and darkened (0..1)
		bool              show = false;
	};

	void Submit(Frame&& a_frame);  // main thread, every frame the table is open
	void Clear();                  // the table closed: drop everything (waits for a frame in flight)

	// Render thread (Present), before the interface draws.
	void Draw(IDXGISwapChain* a_swapChain, ID3D11Device* a_device, ID3D11DeviceContext* a_ctx);
}
