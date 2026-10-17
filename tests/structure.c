#define main writer_main
#include "../src/dwgwriter.c"
#undef main
#include <assert.h>

int main(void) {
    Geometry shapes[2]={{0}};Snapshot source={0};Dwg_Data metadata={0},*d;Dwg_Object_Ref *saved;Snapshot result;
    shapes[0].type=1;strcpy(shapes[0].layer,"MALHA_1");
    shapes[0].values[3]=1;shapes[0].values[6]=1;shapes[0].values[7]=1;shapes[0].values[10]=1;
    shapes[1].type=2;strcpy(shapes[1].layer,"0");shapes[1].values[3]=1;shapes[1].values[6]=1;
    source.items=shapes;source.count=2;source.faces=1;source.circles=1;source.units=6;
    d=document(&source,&metadata);assert(d);assert(structure(d)==0);result=snapshot(d);assert(compare(&source,&result));free(result.items);
    /* The previous writer allowed absent document tables to pass a geometry
     * comparison. This regression check must reject that exact condition. */
    saved=d->header_vars.BLOCK_CONTROL_OBJECT;d->header_vars.BLOCK_CONTROL_OBJECT=NULL;
    assert(structure(d)!=0);d->header_vars.BLOCK_CONTROL_OBJECT=saved;
    {
        Dwg_Object_Entity *e=NULL;unsigned i;
        for(i=0;i<d->num_objects;i++)if(d->object[i].fixedtype==DWG_TYPE__3DFACE){e=d->object[i].tio.entity;break;}
        assert(e);saved=e->layer;e->layer=NULL;assert(structure(d)!=0);e->layer=saved;
    }
    assert(structure(d)==0);dwg_free(d);free(d);
    puts("Estrutura: documento completo, camadas e deteccao de referencias ausentes OK.");return 0;
}
