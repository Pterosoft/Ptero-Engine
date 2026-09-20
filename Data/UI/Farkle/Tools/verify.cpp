// Native RmlUi 6.3 validation and transparent software preview.
// Uses the SAME RML, RCSS, fonts and textures as the engine, without launching it.
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Vertex.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Event.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
struct Texture { int w, h; std::vector<unsigned char> data; };
struct Geometry { std::vector<Rml::Vertex> vertices; std::vector<int> indices; };
struct System : Rml::SystemInterface {
    int problems = 0;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if(type <= Rml::Log::LT_WARNING) { ++problems; std::printf("RML: %s\n",message.c_str()); }
        return true;
    }
};
struct ClickProbe : Rml::EventListener {
    int clicks=0;
    void ProcessEvent(Rml::Event&) override {++clicks;}
};
struct Renderer : Rml::RenderInterface {
    int w, h, missing = 0;
    bool scissor = false;
    Rml::Rectanglei clip;
    ComPtr<IWICImagingFactory> factory;
    std::vector<unsigned char> pixels;
    Renderer(int width,int height):w(width),h(height),pixels(w*h*4,0) {
        CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    }
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> v,Rml::Span<const int> i) override {
        return reinterpret_cast<Rml::CompiledGeometryHandle>(new Geometry{{v.begin(),v.end()},{i.begin(),i.end()}});
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle g) override { delete reinterpret_cast<Geometry*>(g); }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> bytes,Rml::Vector2i size) override {
        return reinterpret_cast<Rml::TextureHandle>(new Texture{size.x,size.y,{bytes.begin(),bytes.end()}});
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size,const Rml::String& source) override {
        ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
        auto path=std::filesystem::u8path(source).wstring();
        if(FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder))) {
            std::printf("MISSING TEXTURE: %s\n",source.c_str()); ++missing; return 0;
        }
        decoder->GetFrame(0,&frame); factory->CreateFormatConverter(&converter);
        converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppPRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
        UINT tw=0,th=0; converter->GetSize(&tw,&th); size={(int)tw,(int)th};
        auto t=new Texture{(int)tw,(int)th,std::vector<unsigned char>(tw*th*4)};
        converter->CopyPixels(nullptr,tw*4,(UINT)t->data.size(),t->data.data());
        return reinterpret_cast<Rml::TextureHandle>(t);
    }
    void ReleaseTexture(Rml::TextureHandle t) override { delete reinterpret_cast<Texture*>(t); }
    void EnableScissorRegion(bool enabled) override { scissor=enabled; }
    void SetScissorRegion(Rml::Rectanglei region) override { clip=region; }
    void RenderGeometry(Rml::CompiledGeometryHandle handle,Rml::Vector2f offset,Rml::TextureHandle texture) override {
        const auto& g=*reinterpret_cast<Geometry*>(handle); const auto* tex=reinterpret_cast<Texture*>(texture);
        for(size_t n=0;n<g.indices.size();n+=3) {
            auto a=g.vertices[g.indices[n]],b=g.vertices[g.indices[n+1]],c=g.vertices[g.indices[n+2]];
            a.position+=offset; b.position+=offset; c.position+=offset;
            auto edge=[](Rml::Vector2f p,Rml::Vector2f q,float x,float y){return (q.x-p.x)*(y-p.y)-(q.y-p.y)*(x-p.x);};
            float area=edge(a.position,b.position,c.position.x,c.position.y);
            if(std::abs(area)<0.0001f) continue;
            if(area<0) { std::swap(b,c); area=-area; }
            int x0=std::max(0,(int)std::floor(std::min({a.position.x,b.position.x,c.position.x})));
            int y0=std::max(0,(int)std::floor(std::min({a.position.y,b.position.y,c.position.y})));
            int x1=std::min(w,(int)std::ceil(std::max({a.position.x,b.position.x,c.position.x})));
            int y1=std::min(h,(int)std::ceil(std::max({a.position.y,b.position.y,c.position.y})));
            if(scissor){x0=std::max(x0,clip.Left());y0=std::max(y0,clip.Top());x1=std::min(x1,clip.Right());y1=std::min(y1,clip.Bottom());}
            auto inclusive=[](Rml::Vector2f p,Rml::Vector2f q){return q.y<p.y || (q.y==p.y && q.x>p.x);};
            for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x) {
                float ea=edge(b.position,c.position,x+0.5f,y+0.5f),eb=edge(c.position,a.position,x+0.5f,y+0.5f),ec=edge(a.position,b.position,x+0.5f,y+0.5f);
                if(ea<0 || eb<0 || ec<0 || (ea==0&&!inclusive(b.position,c.position)) || (eb==0&&!inclusive(c.position,a.position)) || (ec==0&&!inclusive(a.position,b.position))) continue;
                float u=ea/area,v=eb/area,z=ec/area;
                float rgba[4];
                for(int ch=0;ch<4;++ch)rgba[ch]=u*a.colour[ch]+v*b.colour[ch]+z*c.colour[ch];
                if(tex){
                    float tx=(u*a.tex_coord.x+v*b.tex_coord.x+z*c.tex_coord.x)*tex->w-0.5f;
                    float ty=(u*a.tex_coord.y+v*b.tex_coord.y+z*c.tex_coord.y)*tex->h-0.5f;
                    int ix=(int)std::floor(tx),iy=(int)std::floor(ty); float fx=tx-ix,fy=ty-iy;
                    for(int ch=0;ch<4;++ch){
                        auto sample=[&](int sx,int sy){return tex->data[(std::clamp(sy,0,tex->h-1)*tex->w+std::clamp(sx,0,tex->w-1))*4+ch];};
                        float value=(1-fy)*((1-fx)*sample(ix,iy)+fx*sample(ix+1,iy))+fy*((1-fx)*sample(ix,iy+1)+fx*sample(ix+1,iy+1));
                        rgba[ch]*=value/255.f;
                    }
                }
                auto dst=&pixels[(y*w+x)*4];
                for(int ch=0;ch<4;++ch)dst[ch]=(unsigned char)std::clamp(rgba[ch]+dst[ch]*(1-rgba[3]/255.f),0.f,255.f);
            }
        }
    }
    bool Save(const std::filesystem::path& path) {
        auto straight=pixels;
        for(size_t i=0;i<straight.size();i+=4)if(straight[i+3])for(int c=0;c<3;++c)straight[i+c]=(unsigned char)std::min(255,straight[i+c]*255/straight[i+3]);
        ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> props;
        factory->CreateStream(&stream);
        if(FAILED(stream->InitializeFromFilename(path.wstring().c_str(),GENERIC_WRITE)))return false;
        factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder); encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache);
        encoder->CreateNewFrame(&frame,&props);frame->Initialize(props.Get());frame->SetSize(w,h);
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;frame->SetPixelFormat(&format);
        // WIC's PNG encoder may negotiate BGRA instead of RGBA.
        if(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA))
            for(size_t i=0;i<straight.size();i+=4)std::swap(straight[i],straight[i+2]);
        frame->WritePixels(h,w*4,(UINT)straight.size(),straight.data());frame->Commit();return SUCCEEDED(encoder->Commit());
    }
};
int main(int argc,char** argv) {
    if(argc<3){std::puts("verify.exe document.rml preview.png [width height] [dice|hover|dialog|disabled]");return 1;}
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    int width=argc>3?std::atoi(argv[3]):1600,height=argc>4?std::atoi(argv[4]):900;
    System system; Renderer renderer(width,height); Rml::SetSystemInterface(&system);Rml::SetRenderInterface(&renderer);
    if(!Rml::Initialise())return 2;
    auto ctx=Rml::CreateContext("verify",{width,height});
    auto doc=ctx->LoadDocument(std::filesystem::absolute(argv[1]).generic_string());
    if(!doc)return 3;
    doc->Show();ctx->Update();
    // Verify real RmlUi hit testing and event delivery, including button children.
    for(const char* id:{"roll-button","bank-button","clear-button","help-button","pause-button"}) {
        auto e=doc->GetElementById(id); ClickProbe probe; e->AddEventListener("click",&probe);
        auto p=e->GetAbsoluteOffset(); auto size=e->GetBox().GetSize();
        ctx->ProcessMouseMove((int)(p.x+size.x/2),(int)(p.y+size.y/2),0);
        ctx->ProcessMouseButtonDown(0,0);ctx->ProcessMouseButtonUp(0,0);
        if(probe.clicks!=1){std::printf("FAILED click target: %s (%d)\n",id,probe.clicks);++system.problems;}
        e->RemoveEventListener("click",&probe);
    }
    ctx->ProcessMouseMove(0,0,0);doc->Focus();ctx->Update();
    std::string state=argc>5?argv[5]:"idle";
    if(state=="dice") {doc->GetElementById("play-area")->SetClass("has-dice",true);doc->GetElementById("die-1")->SetClass("selected",true);}
    if(state=="dialog")doc->GetElementById("dialog")->SetClass("hidden",false);
    if(state=="disabled")doc->GetElementById("bank-button")->SetAttribute("disabled",true);
    if(state=="hover") {auto b=doc->GetElementById("roll-button");auto p=b->GetAbsoluteOffset();ctx->ProcessMouseMove((int)p.x+30,(int)p.y+15,0);}
    ctx->Update();ctx->Render();
    bool saved=renderer.Save(std::filesystem::absolute(argv[2]));
    const char* ids[]={"score-panel","scoring-panel","log-panel","dice-tray","actions","roll-button","bank-button","clear-button","help-button","pause-button"};
    for(auto id:ids) {
        auto e=doc->GetElementById(id);if(!e){++system.problems;continue;}
        auto p=e->GetAbsoluteOffset();auto s=e->GetBox().GetSize();
        std::printf("%s: %.1f %.1f %.1f %.1f\n",id,p.x,p.y,s.x,s.y);
        if(p.x<0||p.y<0||p.x+s.x>width+1||p.y+s.y>height+1)++system.problems;
    }
    // Sample pixels away from UI must stay transparent.
    for(auto p:std::vector<Rml::Vector2i>{{0,0},{width/2,height/3},{width/2,height/2+height/9},{width-1,height-1}})
        if(renderer.pixels[(p.y*width+p.x)*4+3] && state!="dialog") {std::printf("Unexpected backdrop at %d,%d\n",p.x,p.y);++system.problems;}
    std::printf("Native validation: %d warnings/errors, %d missing textures, saved=%d\n",system.problems,renderer.missing,saved);
    int result=(system.problems||renderer.missing||!saved)?4:0;
    Rml::Shutdown();return result;
}
