#define main frontend_main
#include "../src/dwf3d.cpp"
#undef main
#include <cassert>

int main() {
    // Row-vector scene transforms: child translation followed by parent rotation.
    Matrix move=identity(),rotate=identity();move[12]=2;
    rotate[0]=0;rotate[1]=1;rotate[4]=-1;rotate[5]=0;
    Point q=transform({{1,0,0}},multiply(move,rotate));
    assert(std::abs(q[0])<1e-12 && std::abs(q[1]-3)<1e-12);
    Scene scene;BStreamFileToolkit tk;
    Cylinder cylinder(scene);cylinder.SetAxis(0,0,0,0,0,2);cylinder.SetRadius(1);cylinder.SetCaps(TK_Cylinder::TKCYL_BOTH);cylinder.Execute(tk);
    assert(scene.cylindercount==1 && scene.facecount==384 && scene.pointcount==258);
    Circular circle(scene);circle.SetStart(1,0,0);circle.SetMiddle(0,1,0);circle.SetEnd(-1,0,0);circle.Execute(tk);
    std::vector<Face> faces;std::vector<Circle> circles;flatten(scene.root,identity(),faces,circles);
    assert(faces.size()==384 && circles.size()==1);
    assert(std::abs(circles[0].radius-1)<1e-12 && std::abs(circles[0].center[0])<1e-12);
    scene.root.matrix[0]=2;
    bool failed=false;try{faces.clear();circles.clear();flatten(scene.root,identity(),faces,circles);}catch(const std::runtime_error&){failed=true;}
    assert(failed); // An ellipse must not silently become a circle.
    std::cout<<"Geometria: transformacoes, cilindro com tampas, circulo e escala nao uniforme OK.\n";
}
