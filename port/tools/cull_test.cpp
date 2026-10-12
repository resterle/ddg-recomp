// Checks that CGraphic maps Direct3D cull modes correctly: with D3DCULL_CCW (D3D's default),
// a triangle that is clockwise on screen must be drawn and a counter-clockwise one culled.
//   left  triangle: top-left, top-right, bottom-left  (clockwise on screen)  -> drawn
//   right triangle: top-left, bottom-left, top-right  (counter-clockwise)    -> culled
// Writes snapshot/*.bmp; prints the centre pixel of each half.

#include "../src/unb/Graphic.h"
#include <cstring>

int main(int argc, char **argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    CGraphic::LoadConfig(argc, argv);
    CGraphic::m_Config.windowed = 1;
    CGraphic::m_Config.snapshot = 1;
    SDL_Window *win = SDL_CreateWindow("cull test", 640, 480, 0);
    if (!win || !CGraphic::Init(win)) return 1;

    MeshVertex v[6] = {
        {-0.8f, 0.6f, 0.5f, 0, 0, 0, 0, 0}, {-0.2f, 0.6f, 0.5f, 0, 0, 0, 0, 0}, {-0.8f, -0.6f, 0.5f, 0, 0, 0, 0, 0},
        {0.2f, 0.6f, 0.5f, 0, 0, 0, 0, 0},  {0.2f, -0.6f, 0.5f, 0, 0, 0, 0, 0}, {0.8f, 0.6f, 0.5f, 0, 0, 0, 0, 0},
    };
    CGraphicMesh *mesh = CGraphic::CreateMesh(v, 6);
    MeshConstants c = {};
    for (int i = 0; i < 4; i++) c.localToScreen[i][i] = 1.0f;     // identity: positions are clip space
    c.baseColor[0] = c.baseColor[1] = c.baseColor[2] = c.baseColor[3] = 1.0f;
    c.fog[1] = 1e9f; c.fog[2] = 1.0f;

    for (int frame = 0; frame < 3; frame++) {
        CGraphic::BeginFrame();
        CGraphic::Clear(d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER, 0xff000000, 1.0f);
        CGraphic::SetRenderState(d3d::RS_ZENABLE, 0);
        CGraphic::SetTexture(CGraphic::kMaxTextures, 0, CGraphic::kNoPalette);
        CGraphic::DrawMesh(mesh, MESH_DIFFUSE, d3d::TRIANGLELIST, 0, 2, d3d::CULL_CCW, c);
        CGraphic::EndFrame(frame == 2);
    }
    CGraphic::DestroyMesh(mesh);
    CGraphic::Term();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
