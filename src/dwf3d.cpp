// Independent DWF -> DXF frontend. DWG writing runs in a separate process.
#include "dwf/w3dtk/BStream.h"
#include "dwf/w3dtk/BOpcodeShell.h"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

namespace {
const size_t LIMIT = 256 * 1024 * 1024;
void require(bool ok, const std::string& msg) { if (!ok) throw std::runtime_error(msg); }
#ifdef _WIN32
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, 0, 0);
    require(n > 0, "Caminho UTF-8 invalido.");
    std::vector<wchar_t> b(n); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, b.data(), n);
    return b.data();
}
std::string utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, 0, 0, 0, 0);
    std::vector<char> b(n); WideCharToMultiByte(CP_UTF8, 0, s, -1, b.data(), n, 0, 0);
    return b.data();
}
#endif
FILE* openfile(const std::string& p, const char* mode) {
#ifdef _WIN32
    return _wfopen(wide(p).c_str(), wide(mode).c_str());
#else
    return fopen(p.c_str(), mode);
#endif
}
bool exists(const std::string& p) { FILE* f = openfile(p, "rb"); if (!f) return false; fclose(f); return true; }
std::vector<unsigned char> readfile(const std::string& path) {
    FILE* f = openfile(path, "rb"); require(f != 0, "Nao foi possivel abrir: " + path);
    std::unique_ptr<FILE, int(*)(FILE*)> guard(f, fclose);
    require(fseek(f, 0, SEEK_END) == 0, "Falha ao medir arquivo.");
    long n = ftell(f); require(n >= 0 && size_t(n) <= LIMIT, "Arquivo excede o limite de 256 MiB.");
    rewind(f); std::vector<unsigned char> b(n);
    require(fread(b.data(), 1, b.size(), f) == b.size(), "Leitura incompleta."); return b;
}
void writefile(const std::string& path, const std::string& data) {
    FILE* f = openfile(path, "wb"); require(f != 0, "Nao foi possivel gravar: " + path);
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok; require(ok, "Falha ao gravar arquivo.");
}
uint16_t u16(const std::vector<unsigned char>& b, size_t p) {
    require(p <= b.size() && b.size()-p >= 2, "ZIP truncado."); return b[p] | (b[p+1] << 8);
}
uint32_t u32(const std::vector<unsigned char>& b, size_t p) {
    return uint32_t(u16(b,p)) | (uint32_t(u16(b,p+2)) << 16);
}
struct Entry { std::string name; uint32_t crc, compressed, size, local; uint16_t method, flags; };
struct Package {
    std::vector<unsigned char> bytes; std::vector<Entry> entries; size_t prefix;
    explicit Package(const std::string& p): bytes(readfile(p)), prefix(0) {
        require(bytes.size() >= 22 && std::memcmp(bytes.data(), "(DWF V", 6) == 0, "Entrada nao e um pacote DWF suportado.");
        size_t end = bytes.size()-22, low = bytes.size()>65557 ? bytes.size()-65557 : 0;
        while (true) {
            if (u32(bytes,end)==0x06054b50 && end+22+u16(bytes,end+20)==bytes.size()) break;
            require(end>low, "Diretorio ZIP nao encontrado."); --end;
        }
        require(u16(bytes,end+4)==0 && u16(bytes,end+6)==0 && u16(bytes,end+8)==u16(bytes,end+10), "ZIP multidisco nao suportado.");
        uint32_t size=u32(bytes,end+12), offset=u32(bytes,end+16);
        require(size<=end && offset<=end-size, "Diretorio ZIP invalido ou ZIP64 nao suportado.");
        prefix=end-size-offset; size_t pos=prefix+offset;
        for (unsigned i=0; i<u16(bytes,end+10); ++i) {
            require(u32(bytes,pos)==0x02014b50, "Entrada ZIP invalida.");
            unsigned n=u16(bytes,pos+28), extra=u16(bytes,pos+30), comment=u16(bytes,pos+32);
            require(pos+46+n+extra+comment<=end, "Diretorio ZIP truncado.");
            Entry e; e.flags=u16(bytes,pos+8); e.method=u16(bytes,pos+10); e.crc=u32(bytes,pos+16);
            e.compressed=u32(bytes,pos+20); e.size=u32(bytes,pos+24); e.local=u32(bytes,pos+42);
            e.name=std::string(bytes.begin()+pos+46,bytes.begin()+pos+46+n); entries.push_back(e);
            pos+=46+n+extra+comment;
        }
        require(pos==end, "Tamanho do diretorio ZIP inconsistente.");
    }
    std::vector<unsigned char> extract(const Entry& e) const {
        require(!(e.flags&1) && e.size<=LIMIT && e.compressed<=LIMIT, "Recurso criptografado ou muito grande.");
        size_t p=prefix+e.local; require(u32(bytes,p)==0x04034b50, "Cabecalho ZIP invalido.");
        require(u16(bytes,p+8)==e.method, "Metodo ZIP inconsistente.");
        size_t n=u16(bytes,p+26); p+=30+n+u16(bytes,p+28);
        require(p<=bytes.size() && e.compressed<=bytes.size()-p, "Recurso ZIP truncado.");
        std::vector<unsigned char> result(e.size);
        if (e.method==0) {
            require(e.size==e.compressed, "Tamanho ZIP inconsistente.");
            std::copy(bytes.begin()+p,bytes.begin()+p+e.size,result.begin());
        } else {
            require(e.method==8, "Compressao ZIP nao suportada.");
            z_stream s; std::memset(&s,0,sizeof(s));
            s.next_in=const_cast<Bytef*>(bytes.data()+p); s.avail_in=e.compressed;
            unsigned char empty=0; s.next_out=result.empty()?&empty:result.data(); s.avail_out=std::max(1u,e.size);
            require(inflateInit2(&s,-MAX_WBITS)==Z_OK, "Falha no descompressor ZIP.");
            int status=inflate(&s,Z_FINISH); inflateEnd(&s);
            require(status==Z_STREAM_END && s.total_out==e.size && s.total_in==e.compressed, "Recurso ZIP corrompido.");
        }
        require(crc32(0,result.data(),result.size())==e.crc, "CRC do recurso DWF incorreto."); return result;
    }
};
typedef std::array<double,16> Matrix;
typedef std::array<double,3> Point;
Matrix identity() { return {{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}}; }
Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix c={{0}}; for(int i=0;i<4;++i) for(int j=0;j<4;++j) for(int k=0;k<4;++k) c[i*4+j]+=a[i*4+k]*b[k*4+j]; return c;
}
Point transform(const Point& p,const Matrix& m) {
    Point q; double w=p[0]*m[3]+p[1]*m[7]+p[2]*m[11]+m[15]; require(std::isfinite(w)&&std::abs(w)>1e-15,"Transformacao invalida.");
    for(int j=0;j<3;++j) { q[j]=(p[0]*m[j]+p[1]*m[4+j]+p[2]*m[8+j]+m[12+j])/w; require(std::isfinite(q[j]),"Coordenada invalida."); } return q;
}
struct Mesh { std::vector<Point> points; std::vector<std::vector<int> > faces; int number; };
struct Circle { Point center, normal; double radius; };
Point sub(const Point& a,const Point& b) {return {{a[0]-b[0],a[1]-b[1],a[2]-b[2]}};}
double dot(const Point& a,const Point& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Point cross(const Point& a,const Point& b) {return {{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}};}
Point scale(const Point& p,double a) {return {{p[0]*a,p[1]*a,p[2]*a}};}
Point unit(const Point& p) {double n=std::sqrt(dot(p,p));require(n>1e-15,"Circulo degenerado.");return scale(p,1/n);}
struct Node { Matrix matrix; std::vector<std::unique_ptr<Node> > children; std::vector<Mesh> meshes; std::vector<Circle> circles; Node():matrix(identity()){} };
struct Scene {
    Node root; std::vector<Node*> stack; size_t meshcount=0, pointcount=0, facecount=0, cylindercount=0;
    Scene() { stack.push_back(&root); }
    Node& current() { return *stack.back(); }
};
class Open:public TK_Open_Segment { Scene& s; public: explicit Open(Scene& s):s(s){} TK_Status Execute(BStreamFileToolkit&) {
    require(s.stack.size()<4096,"Hierarquia muito profunda."); s.current().children.emplace_back(new Node); s.stack.push_back(s.current().children.back().get()); return TK_Normal;
}};
class Close:public TK_Close_Segment { Scene& s; public: explicit Close(Scene& s):s(s){} TK_Status Execute(BStreamFileToolkit&) {
    require(s.stack.size()>1,"Fechamento de segmento invalido."); s.stack.pop_back(); return TK_Normal;
}};
class Modelling:public TK_Matrix { Scene& s; public: explicit Modelling(Scene& s):TK_Matrix(TKE_Modelling_Matrix),s(s){} TK_Status Execute(BStreamFileToolkit&) {
    for(int i=0;i<16;++i) { require(std::isfinite(GetMatrix()[i]),"Matriz invalida."); s.current().matrix[i]=GetMatrix()[i]; } return TK_Normal;
}};
class Circular:public TK_Circle { Scene& s; public: explicit Circular(Scene& s):TK_Circle(TKE_Circle),s(s){} TK_Status Execute(BStreamFileToolkit&) {
    Point a={{GetStart()[0],GetStart()[1],GetStart()[2]}},b={{GetMiddle()[0],GetMiddle()[1],GetMiddle()[2]}},c={{GetEnd()[0],GetEnd()[1],GetEnd()[2]}};
    Point u=sub(b,a),v=sub(c,a),w=cross(u,v); double w2=dot(w,w);require(w2>1e-30,"Circulo degenerado.");
    Point x=cross(v,w),y=cross(w,u),center;
    for(int j=0;j<3;++j) center[j]=a[j]+(dot(u,u)*x[j]+dot(v,v)*y[j])/(2*w2);
    if(GetCenter()) for(int j=0;j<3;++j) center[j]=GetCenter()[j];
    double r=std::sqrt(dot(sub(a,center),sub(a,center)));require(r>0 && std::isfinite(r),"Raio invalido.");
    require(std::abs(std::sqrt(dot(sub(b,center),sub(b,center)))-r)<1e-5*std::max(r,1e-6) && std::abs(std::sqrt(dot(sub(c,center),sub(c,center)))-r)<1e-5*std::max(r,1e-6),"Pontos do circulo inconsistentes.");
    s.current().circles.push_back({center,unit(w),r});return TK_Normal;
}};
class Cylinder:public TK_Cylinder { Scene& s; public: explicit Cylinder(Scene& s):s(s){} TK_Status Execute(BStreamFileToolkit&) {
    const int sides=128; const double pi=std::acos(-1.0); double r=GetRadius();
    require(std::isfinite(r)&&r>0 && GetCaps()>=0 && GetCaps()<=3,"Cilindro invalido.");
    Point a={{GetStart()[0],GetStart()[1],GetStart()[2]}},b={{GetEnd()[0],GetEnd()[1],GetEnd()[2]}};
    Point axis=unit(sub(b,a)),x=unit(cross(std::abs(axis[2])<0.9?Point{{0,0,1}}:Point{{0,1,0}},axis)),y=cross(axis,x);
    Mesh m; m.number=++s.meshcount; ++s.cylindercount;
    for(int cap=0;cap<2;++cap) for(int i=0;i<sides;++i) {double angle=2*pi*i/sides;Point p=cap?b:a;for(int j=0;j<3;++j)p[j]+=r*(std::cos(angle)*x[j]+std::sin(angle)*y[j]);m.points.push_back(p);}
    m.points.push_back(a);m.points.push_back(b);
    for(int i=0;i<sides;++i) {int next=(i+1)%sides;m.faces.push_back({i,next,next+sides,i+sides});
        if(GetCaps()&TKCYL_FIRST)m.faces.push_back({2*sides,next,i});
        if(GetCaps()&TKCYL_SECOND)m.faces.push_back({2*sides+1,i+sides,next+sides});
    }
    s.pointcount+=m.points.size();s.facecount+=m.faces.size();require(s.pointcount<=10000000&&s.facecount<=2000000,"Modelo muito grande.");
    s.current().meshes.push_back(std::move(m));return TK_Normal;
}};
class Shell:public TK_Shell { Scene& s; public: explicit Shell(Scene& s):s(s){} TK_Status Execute(BStreamFileToolkit&) {
    require(GetLodLevel()==0 && (GetSubop()&TKSH_FIRSTPASS) && !(GetSubop()&TKSH_BOUNDING_ONLY) && !GetSubop2(),"LOD/refinamento de malha nao suportado.");
    require(GetPointCount()>0 && GetPointCount()<10000000,"Quantidade de vertices invalida.");
    Mesh m; m.number=++s.meshcount; s.pointcount+=GetPointCount(); require(s.pointcount<=10000000,"Modelo muito grande.");
    for(int i=0;i<GetPointCount();++i) m.points.push_back({{GetPoints()[3*i],GetPoints()[3*i+1],GetPoints()[3*i+2]}});
    int n=GetFacesLength(); const int* f=GetFaces();
    for(int pos=0;pos<n;) {
        int count=f[pos++]; require(count>=3 && count<=n-pos,"Face com furos ou lista de faces invalida.");
        std::vector<int> face; for(int j=0;j<count;++j) { int v=f[pos++]; require(v>=0&&v<GetPointCount(),"Indice de vertice invalido."); face.push_back(v); }
        if(GetSubop()&TKSH_TRISTRIPS) {
            for(int j=0;j<count-2;++j) m.faces.push_back(j%2?std::vector<int>{face[j+1],face[j],face[j+2]}:std::vector<int>{face[j],face[j+1],face[j+2]});
        } else { require(count<=4,"Poligonos com mais de quatro vertices exigem triangulacao adicional."); m.faces.push_back(face); }
    }
    s.facecount+=m.faces.size(); require(s.facecount<=2000000,"Modelo excede o limite de faces.");
    s.current().meshes.push_back(std::move(m)); return TK_Normal;
}};
// Fail before reading unsupported geometry rather than silently dropping it.
class Unsupported:public BBaseOpcodeHandler { public: explicit Unsupported(unsigned char op):BBaseOpcodeHandler(op){} TK_Status Write(BStreamFileToolkit&) {return TK_Error;} TK_Status Read(BStreamFileToolkit&) {
    throw std::runtime_error("Recurso W3D nao suportado (opcode "+std::to_string(unsigned(m_opcode))+"). Conversao cancelada para evitar perda silenciosa de geometria.");
}};
struct Face { std::array<Point,4> points; int mesh; };
void flatten(const Node& node,const Matrix& parent,std::vector<Face>& out,std::vector<Circle>& circles) {
    Matrix matrix=multiply(node.matrix,parent);
    for(const Mesh& m:node.meshes) for(const auto& f:m.faces) {
        Face face; face.mesh=m.number;
        for(int j=0;j<4;++j) face.points[j]=transform(m.points[f[std::min(j,int(f.size())-1)]],matrix);
        out.push_back(face);
    }
    for(const auto& c:node.circles) {
        Point x=unit(cross(std::abs(c.normal[2])<0.9?Point{{0,0,1}}:Point{{0,1,0}},c.normal)),y=cross(c.normal,x);
        Point tx={{0}},ty={{0}};
        for(int j=0;j<3;++j) for(int k=0;k<3;++k) {tx[j]+=x[k]*matrix[4*k+j];ty[j]+=y[k]*matrix[4*k+j];}
        double rx=std::sqrt(dot(tx,tx)),ry=std::sqrt(dot(ty,ty));
        require(rx>0 && std::abs(rx-ry)<1e-7*rx && std::abs(dot(tx,ty))<1e-7*rx*ry,"Circulo com escala nao uniforme exige exportacao como elipse.");
        circles.push_back({transform(c.center,matrix),unit(cross(tx,ty)),c.radius*rx});
    }
    for(const auto& child:node.children) flatten(*child,matrix,out,circles);
}
std::string dxf(const std::vector<Face>& faces,const std::vector<Circle>& circles,int units,size_t meshes) {
    Point lo={{INFINITY,INFINITY,INFINITY}},hi={{-INFINITY,-INFINITY,-INFINITY}};
    for(const auto& f:faces) for(const auto& p:f.points) for(int j=0;j<3;++j) { lo[j]=std::min(lo[j],p[j]);hi[j]=std::max(hi[j],p[j]); }
    for(const auto& c:circles) for(int j=0;j<3;++j) {double d=c.radius*std::sqrt(std::max(0.0,1-c.normal[j]*c.normal[j]));lo[j]=std::min(lo[j],c.center[j]-d);hi[j]=std::max(hi[j],c.center[j]+d);}
    std::ostringstream o; o.imbue(std::locale::classic()); o<<std::setprecision(17);
    o<<"0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1015\n9\n$INSUNITS\n70\n"<<units<<"\n";
    o<<"9\n$MEASUREMENT\n70\n1\n9\n$EXTMIN\n10\n"<<lo[0]<<"\n20\n"<<lo[1]<<"\n30\n"<<lo[2]<<"\n9\n$EXTMAX\n10\n"<<hi[0]<<"\n20\n"<<hi[1]<<"\n30\n"<<hi[2]<<"\n0\nENDSEC\n";
    o<<"0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nLAYER\n5\n1\n100\nAcDbSymbolTable\n70\n"<<meshes+1<<"\n";
    for(size_t i=0;i<=meshes;++i) o<<"0\nLAYER\n100\nAcDbSymbolTableRecord\n100\nAcDbLayerTableRecord\n2\n"<<(i?"MALHA_"+std::to_string(i):"0")<<"\n70\n0\n62\n7\n6\nCONTINUOUS\n";
    o<<"0\nENDTAB\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n";
    for(const auto& f:faces) {
        o<<"0\n3DFACE\n100\nAcDbEntity\n8\nMALHA_"<<f.mesh<<"\n100\nAcDbFace\n";
        for(int j=0;j<4;++j) for(int k=0;k<3;++k) o<<10+10*k+j<<"\n"<<f.points[j][k]<<"\n";
        o<<"70\n0\n";
    }
    for(const auto& c:circles) {
        Point x=unit(cross(std::abs(c.normal[0])<1.0/64 && std::abs(c.normal[1])<1.0/64?Point{{0,1,0}}:Point{{0,0,1}},c.normal));
        Point y=cross(c.normal,x);
        o<<"0\nCIRCLE\n100\nAcDbEntity\n8\n0\n100\nAcDbCircle\n10\n"<<dot(c.center,x)<<"\n20\n"<<dot(c.center,y)<<"\n30\n"<<dot(c.center,c.normal)<<"\n40\n"<<c.radius<<"\n210\n"<<c.normal[0]<<"\n220\n"<<c.normal[1]<<"\n230\n"<<c.normal[2]<<"\n";
    }
    o<<"0\nENDSEC\n0\nEOF\n"; return o.str();
}
int run(std::vector<std::string> args) {
    bool interactive=false;
#ifdef _WIN32
    if(args.size()==1) {
        wchar_t path[32768]={0}; OPENFILENAMEW ofn={}; ofn.lStructSize=sizeof(ofn); ofn.lpstrFile=path; ofn.nMaxFile=32768;
        ofn.lpstrFilter=L"Desenhos DWF\0*.dwf\0\0"; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
        if(!GetOpenFileNameW(&ofn)) return 0; args.push_back(utf8(path)); interactive=true;
    }
#endif
    if(args.size()<2 || args[1]=="--help") {
        std::cout<<"Uso: dwf3d arquivo.dwf [saida.dwg|saida.dxf]\nPreserva faces 3D; nao reconstroi solidos parametricos, cores ou propriedades BIM.\n"; return args.size()<2?2:0;
    }
    require(args.size()<=3,"Argumentos em excesso.");
    std::string input=args[1],output=args.size()==3?args[2]:input.substr(0,input.find_last_of('.'))+".dwg";
    std::string ext=output.substr(output.find_last_of('.')+1); std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    require(ext=="dxf"||ext=="dwg","Saida deve ter extensao .dwg ou .dxf.");
    require(!exists(output),"Arquivo de saida ja existe. Escolha outro nome: "+output);
    Package package(input); std::vector<unsigned char> w3d; int units=0; size_t streams=0,descriptors=0;
    for(const Entry& e:package.entries) {
        if(e.name.size()>=4 && e.name.substr(e.name.size()-4)==".w3d") { w3d=package.extract(e); ++streams; }
        if(e.name.size()>=14 && e.name.substr(e.name.size()-14)=="descriptor.xml") {
            auto data=package.extract(e); std::string xml(data.begin(),data.end());
            if(xml.find("eModel:Space")!=std::string::npos) {
                ++descriptors; std::smatch match;
                require(std::regex_search(xml,match,std::regex("<eModel:Units\\s+type=\"([^\"]+)\"")),"Unidades do modelo ausentes.");
                const std::string unit=match[1];
                if(unit=="m") units=6; else if(unit=="mm") units=4; else if(unit=="cm") units=5; else if(unit=="ft") units=2; else if(unit=="in") units=1;
                else throw std::runtime_error("Unidade nao suportada: "+unit);
                require(xml.find("transform=\"1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\"")!=std::string::npos,"Transformacao de recurso nao suportada.");
            }
        }
    }
    require(streams==1 && descriptors==1 && units,"Necessario exatamente um modelo W3D com unidades e transformacao suportadas.");
    Scene scene; BStreamFileToolkit tk;
    tk.SetOpcodeHandler(TKE_Open_Segment,new Open(scene)); tk.SetOpcodeHandler(TKE_Close_Segment,new Close(scene));
    tk.SetOpcodeHandler(TKE_Modelling_Matrix,new Modelling(scene)); tk.SetOpcodeHandler(TKE_Shell,new Shell(scene));
    tk.SetOpcodeHandler(TKE_Circle,new Circular(scene));
    tk.SetOpcodeHandler(TKE_Cylinder,new Cylinder(scene));
    const unsigned unsupported[]={TKE_Reopen_Segment,TKE_Include_Segment,TKE_Style_Segment,TKE_Reference,TKE_Repeat_Object,TKE_LOD,TKE_Delete_Object,
        TKE_Infinite_Line,TKE_Area_Light,TKE_Circular_Arc,TKE_Circular_Chord,TKE_Circular_Wedge,TKE_Cutting_Plane,TKE_Ellipse,TKE_Elliptical_Arc,TKE_Grid,TKE_Image,
        TKE_Line,TKE_Marker,TKE_Mesh,TKE_NURBS_Curve,TKE_NURBS_Surface,TKE_PolyCylinder,TKE_Polygon,TKE_Polyline,TKE_PolyPolyline,TKE_Sphere,TKE_Text,TKE_Text_With_Encoding,TKE_External_Reference};
    for(unsigned op:unsupported) tk.SetOpcodeHandler(op,new Unsupported(op));
    TK_Status status=tk.ParseBuffer(reinterpret_cast<char*>(w3d.data()),int(w3d.size()));
    require(status==TK_Complete,"Leitura W3D nao concluida (status "+std::to_string(status)+").");
    require(scene.stack.size()==1 && scene.meshcount>0,"Modelo vazio ou hierarquia incompleta.");
    std::vector<Face> faces; std::vector<Circle> circles; flatten(scene.root,identity(),faces,circles); require(!faces.empty(),"Modelo sem faces.");
    std::cout<<"Malhas: "<<scene.meshcount<<" (cilindros discretizados: "<<scene.cylindercount<<"); vertices: "<<scene.pointcount<<"; faces 3D: "<<faces.size()<<"; circulos: "<<circles.size()<<"; INSUNITS: "<<units<<"\n";
    std::string content=dxf(faces,circles,units,scene.meshcount);
    if(ext=="dxf") writefile(output,content);
    else {
#ifdef _WIN32
        wchar_t module[32768]; require(GetModuleFileNameW(0,module,32768)>0,"Falha ao localizar executavel.");
        std::wstring folder=module; folder=folder.substr(0,folder.find_last_of(L"\\/"));
        std::wstring writer=folder+L"\\dwgwriter.exe"; require(exists(utf8(writer.c_str())),"dwgwriter.exe ausente. Extraia o pacote completo.");
        wchar_t tempdir[32768],tempfile[32768]; require(GetTempPathW(32768,tempdir)>0,"Falha no diretorio temporario.");
        require(GetTempFileNameW(tempdir,L"dwf",0,tempfile)!=0,"Falha no arquivo temporario.");
        std::wstring temp=tempfile, dwgtemp=temp+L".dwg";
        struct Cleanup { std::wstring a,b; ~Cleanup(){DeleteFileW(a.c_str());DeleteFileW(b.c_str());} } cleanup{temp,dwgtemp};
        writefile(utf8(temp.c_str()),content);
        std::wstring command=L"\""+writer+L"\" \""+temp+L"\" \""+dwgtemp+L"\"";
        std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0); STARTUPINFOW si={};si.cb=sizeof(si); PROCESS_INFORMATION pi={};
        require(CreateProcessW(writer.c_str(),cmd.data(),0,0,FALSE,0,0,0,&si,&pi)!=0,"Falha ao iniciar gravador DWG.");
        WaitForSingleObject(pi.hProcess,INFINITE); DWORD result=1;GetExitCodeProcess(pi.hProcess,&result);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        require(result==0,"Falha na gravacao/validacao DWG. Nenhum resultado foi publicado.");
        require(CopyFileW(dwgtemp.c_str(),wide(output).c_str(),TRUE)!=0,"Falha ao copiar DWG para destino (arquivo existente ou acesso negado).");
#else
        throw std::runtime_error("Nesta compilacao Linux, use saida.dxf e depois dwgwriter entrada.dxf saida.dwg.");
#endif
    }
    std::cout<<"Salvo: "<<output<<"\n";
#ifdef _WIN32
    if(interactive) MessageBoxW(0,wide("Conversao concluida: "+output).c_str(),L"DWF 3D",MB_OK);
#endif
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    try { std::vector<std::string> args; for(int i=0;i<argc;++i) args.push_back(utf8(argv[i])); return run(args); }
    catch(const std::exception& e) {std::cerr<<"Erro: "<<e.what()<<"\n";if(argc==1)MessageBoxW(0,wide(e.what()).c_str(),L"Falha na conversao",MB_OK|MB_ICONERROR);return 1;}
}
#else
int main(int argc,char** argv) {try {return run(std::vector<std::string>(argv,argv+argc));}catch(const std::exception& e){std::cerr<<"Erro: "<<e.what()<<"\n";return 1;}}
#endif
