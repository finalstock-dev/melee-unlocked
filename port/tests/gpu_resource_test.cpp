// Exercise the production renderer with deliberately tiny descriptor/upload pools.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include "gx_d3d12.h"
#include "gx_shader.h"
#include "gx_texture.h"
#include "video_background.h"
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace host {
void log(const char* fmt, ...) { va_list args; va_start(args, fmt); vprintf(fmt,args); va_end(args); puts(""); }
[[noreturn]] void die(const char* fmt, ...) { char buf[1024]; va_list args; va_start(args,fmt); vsnprintf(buf,sizeof buf,fmt,args); va_end(args); throw std::runtime_error(buf); }
// The renderer reaches back into the window for fullscreen changes; this test owns a bare HWND.
bool window_take_fullscreen_toggle() { return false; }
void window_set_fullscreen(bool) {}
void window_set_title(const wchar_t*) {}
}
namespace slippi { void request_widescreen(bool) {} }
// This renderer regression test deliberately links the production D3D12 backend without Media
// Foundation. Keep its video-background hooks inert so it exercises descriptor/upload behavior.
namespace gx::video_bg {
void begin_frame(uint8_t, uint8_t) {}
bool wants_texture_names() { return false; }
std::shared_ptr<const Frame> lookup(const std::string&, uint32_t, uint32_t, int*) { return {}; }
void set_enabled(bool) {}
void report_backend_failure(int, const std::string&) {}
}
static void check(bool b, const char* why) { if(!b) throw std::runtime_error(why); }
static void f32(uint32_t& out, float f) { memcpy(&out,&f,4); }
static gx::TextureRef texture(uint32_t addr, uint8_t r, uint8_t b) {
  gx::TextureRef t; t.addr=addr; t.width=t.height=4; t.format=6; t.used=true;
  auto data=std::make_shared<gx::TextureSnapshot>(); data->image.resize(64);
  for(int i=0;i<16;++i) { data->image[i*2]=255; data->image[i*2+1]=r; data->image[32+i*2]=0; data->image[33+i*2]=b; }
  data->hash=gx::hash_bytes(data->image.data(),data->image.size()); t.data=data; return t;
}
int main() {
  HWND window=nullptr;
  try {
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"MeleeGpuResourceTest";
    RegisterClassW(&wc);
    window=CreateWindowW(wc.lpszClassName,L"GPU resource regression",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
    check(window!=nullptr,"hidden window creation");
    gx::D3D12Options options; options.efb_scale=1; options.capture_frame=6; options.capture_path="gpu-resource-test.ppm";
    std::unique_ptr<gx::Backend> renderer(gx::create_d3d12_backend(window,640,480,options));
    gx::Frame frame; frame.sequence=1;
    // The first red quad must survive every subsequent descriptor/page rollover.
    auto red=texture(0x1000,255,0), blue=texture(0x2000,0,255);
    gx::DrawCall d{}; d.primitive=0x80; d.vertex_count=4; d.components=gx::VB_HAS_UV0;
    d.posMatrices[0]=d.posMatrices[5]=d.posMatrices[10]=1;
    d.matrix_index_a=60u<<6; // identity texture matrix
    d.xf_regs[0x26]=1; d.xf_regs[0x3F]=1; d.xf_regs[0x40]=5u<<7;
    f32(d.xf_regs[0x1A],320); f32(d.xf_regs[0x1B],-240);
    f32(d.xf_regs[0x1C],16777215); f32(d.xf_regs[0x1D],662); f32(d.xf_regs[0x1E],582); f32(d.xf_regs[0x1F],16777215);
    f32(d.xf_regs[0x20],1); f32(d.xf_regs[0x22],1); f32(d.xf_regs[0x24],1);
    d.bp.reg[gx::BP_GENMODE]=1;
    d.bp.reg[gx::BP_BLENDMODE]=24;
    d.bp.reg[gx::BP_SCISSORTL]=(342u<<12)|342u;
    d.bp.reg[gx::BP_SCISSORBR]=((342u+639)<<12)|(342u+479);
    d.bp.reg[gx::BP_SCISSOROFFSET]=(171u<<10)|171u;
    d.bp.reg[gx::BP_TREF]=64;
    d.bp.reg[gx::BP_TEV_KSEL]=4; d.bp.reg[gx::BP_TEV_KSEL+1]=14;
    d.bp.reg[gx::BP_TEV_COLOR_ENV]=0x8fff8;
    d.bp.reg[gx::BP_TEV_ALPHA_ENV]=0x8ffc0;
    d.bp.reg[gx::BP_ALPHACOMPARE]=(7u<<16)|(7u<<19);
    gx::EfbCopy clear{}; clear.src_w=640; clear.src_h=480; clear.clear=true; clear.clear_color=0xff000000; clear.clear_z=0xffffff;
    // A copy with clear initializes the EFB before the actual test draws.
    clear.dest_addr=0x3000; frame.copies.push_back(clear); frame.commands.push_back({gx::FrameCommand::Copy,0});
    for(unsigned i=0;i<40;++i) {
      d.first_vertex=(uint32_t)frame.vertices.size(); d.textures[0]=i==0?red:texture(0x4000+i*0x100,0,255);
      d.textures[0].mode1=i<<8; // distinct sampler sets force sampler rollover
      float left=i==0?-1.f:0.f, right=i==0?0.f:1.f;
      const float xy[4][2]={{left,-1},{right,-1},{right,1},{left,1}};
      for(auto& p:xy) { gx::Vertex v{}; v.pos[0]=p[0]; v.pos[1]=p[1]; v.uv[0][0]=v.uv[0][1]=0.5f; frame.vertices.push_back(v); }
      frame.draws.push_back(d); frame.commands.push_back({gx::FrameCommand::Draw,i});
    }
    gx::EfbCopy present{}; present.to_xfb=true; present.src_w=640; present.src_h=480; present.y_scale=1;
    frame.copies.push_back(present); frame.commands.push_back({gx::FrameCommand::Copy,1});
    for (unsigned n=0; n<6; ++n) { frame.sequence=n+1; renderer->submit_frame(frame); }
    const uint64_t owner_a = frame.draws[0].cached_pipeline_owner;
    // Replay the same immutable packets through a new device/backend: their
    // cached PSO pointers must never be reused after the original owner dies.
    renderer.reset();
    renderer.reset(gx::create_d3d12_backend(window,640,480,options));
    uint32_t warmed = 0; gx::d3d12_stats(renderer.get(), nullptr, &warmed, nullptr);
    check(warmed > 0, "recorded pipelines prewarm before the next draw");
    for (unsigned n=0; n<6; ++n) { frame.sequence=n+1; renderer->submit_frame(frame); }
    const uint64_t owner_b = frame.draws[0].cached_pipeline_owner;
    // A draw's cached pipeline belongs to one backend AND one motion-vector (DLSS) variant: the
    // variant changes shader generation and the render target count, so a pipeline built for the
    // other one must never be handed back after DLSS is switched on or off. The owner is
    // backend_id*2 + variant, so the no-motion-vector variant is always even and two backends
    // created in sequence step by two. Without the variant in the owner the step was one.
    printf("pso cache owners: %llu then %llu\n",(unsigned long long)owner_a,(unsigned long long)owner_b);
    check(owner_a!=0&&owner_b!=0,"draws resolved a real pipeline");
    check(owner_a%2==0&&owner_b%2==0,"pipelines without motion vectors cache under the even owner");
    check(owner_b==owner_a+2,"each backend owns one cache identity per motion-vector variant");

    // Texture generator 7 must read Vertex::texmtx[7] (offset 104), not generator 6's index.
    static_assert(offsetof(gx::Vertex,texmtx)+7==104,"texmtx[7] byte offset");
    {
      gx::DrawCall t=d;
      t.components|=(gx::VB_HAS_TEXMTXIDX0<<6)|(gx::VB_HAS_TEXMTXIDX0<<7);   // per-vertex matrix index for texgens 6 and 7
      t.xf_regs[0x3F]=8;                                                     // eight texture generators
      for(int i=0;i<8;++i) t.xf_regs[0x40+i]=5u<<7;                          // all of them from TEXCOORD0
      const std::string vs=gx::generate_vertex_shader(gx::make_vs_uid(t));
      check(vs.find("uint blend_index7 : BLENDINDICES2")!=std::string::npos,"texgen 7 has its own vertex input");
      const size_t six=vs.find("o.tex6.xyz"), seven=vs.find("o.tex7.xyz");
      check(six!=std::string::npos&&seven!=std::string::npos,"eight texture generators emitted");
      // Each assignment is preceded by the "int tmp = int(<input>);" line naming the input it reads.
      const std::string src6=vs.substr(vs.rfind("int tmp = int(",six),40), src7=vs.substr(vs.rfind("int tmp = int(",seven),40);
      printf("texgen 6 reads %s\ntexgen 7 reads %s\n",src6.c_str(),src7.c_str());
      check(src6!=src7,"texgens 6 and 7 read different matrix indices");
      check(src7.find("blend_index7")!=std::string::npos,"texgen 7 reads texmtx[7]");
      // And the input layout really feeds it: D3D12 refuses a pipeline whose vertex shader reads an
      // element the layout does not provide, so building this draw's pipeline proves the wiring.
      gx::Frame texgen_frame; texgen_frame.sequence=7;
      texgen_frame.copies.push_back(clear); texgen_frame.commands.push_back({gx::FrameCommand::Copy,0});
      t.first_vertex=0; t.textures[0]=red;
      const float xy[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
      for(auto& p:xy) { gx::Vertex v{}; v.pos[0]=p[0]; v.pos[1]=p[1]; v.uv[0][0]=v.uv[0][1]=0.5f; v.texmtx[6]=57; v.texmtx[7]=60; texgen_frame.vertices.push_back(v); }
      t.cached_pipeline=nullptr; t.cached_pipeline_owner=0;
      texgen_frame.draws.push_back(t); texgen_frame.commands.push_back({gx::FrameCommand::Draw,0});
      for(unsigned n=0;n<6;++n) { texgen_frame.sequence=7+n; renderer->submit_frame(texgen_frame); }
      check(texgen_frame.draws[0].cached_pipeline!=nullptr,"eight-texgen pipeline built against the new input layout");
    }
    renderer.reset(); DestroyWindow(window); window=nullptr;
    std::ifstream file(options.capture_path,std::ios::binary); std::string magic; int w,h,max;
    file>>magic>>w>>h>>max; file.get(); check(magic=="P6"&&w==640&&h==480&&max==255,"capture header");
    std::vector<unsigned char> image(w*h*3); file.read((char*)image.data(),image.size()); check((size_t)file.gcount()==image.size(),"capture size");
    const auto* l=&image[(240*w+160)*3]; const auto* r=&image[(240*w+480)*3];
    printf("left=%u,%u,%u right=%u,%u,%u\n",l[0],l[1],l[2],r[0],r[1],r[2]);
    check(l[0]>240&&l[2]<10,"earlier red draw survives descriptor/upload rollover");
    check(r[2]>240&&r[0]<10,"later blue draw has independent descriptors");
    puts("D3D12 descriptor and upload lifetime regression passed"); return 0;
  } catch(const std::exception& e) { if(window)DestroyWindow(window); fprintf(stderr,"%s\n",e.what()); return 1; }
}
