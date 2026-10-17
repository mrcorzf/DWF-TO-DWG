/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone LibreDWG frontend. No Autodesk toolkit is linked here.
 */
#include "config.h"
#include "dwg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned type; double values[12]; } Geometry;
typedef struct { Geometry *items; size_t count, faces, circles; unsigned units; } Snapshot;

static Snapshot snapshot(Dwg_Data *dwg) {
    Snapshot s = {0}; size_t i;
    s.items = (Geometry *)calloc(dwg->num_objects, sizeof(Geometry));
    if (!s.items) {fprintf(stderr,"Sem memoria.\n");exit(1);}
    s.units=dwg->header_vars.INSUNITS;
    for(i=0;i<dwg->num_objects;++i) {
        Dwg_Object *o=&dwg->object[i]; Geometry *g=&s.items[s.count];
        if(o->fixedtype==DWG_TYPE__3DFACE) {
            Dwg_Entity__3DFACE *f=o->tio.entity->tio._3DFACE;
            g->type=1;
            g->values[0]=f->corner1.x;g->values[1]=f->corner1.y;g->values[2]=f->corner1.z;
            g->values[3]=f->corner2.x;g->values[4]=f->corner2.y;g->values[5]=f->corner2.z;
            g->values[6]=f->corner3.x;g->values[7]=f->corner3.y;g->values[8]=f->corner3.z;
            g->values[9]=f->corner4.x;g->values[10]=f->corner4.y;g->values[11]=f->corner4.z;
            ++s.count;++s.faces;
        } else if(o->fixedtype==DWG_TYPE_CIRCLE) {
            Dwg_Entity_CIRCLE *c=o->tio.entity->tio.CIRCLE;
            g->type=2;g->values[0]=c->center.x;g->values[1]=c->center.y;g->values[2]=c->center.z;
            g->values[3]=c->radius;g->values[4]=c->extrusion.x;g->values[5]=c->extrusion.y;g->values[6]=c->extrusion.z;
            ++s.count;++s.circles;
        }
    }
    return s;
}
static int compare(const Snapshot *a,const Snapshot *b) {
    size_t i,j;
    if(a->count!=b->count||a->faces!=b->faces||a->circles!=b->circles||a->units!=b->units) return 0;
    for(i=0;i<a->count;++i) {
        if(a->items[i].type!=b->items[i].type) return 0;
        for(j=0;j<12;++j) {
            double x=a->items[i].values[j],y=b->items[i].values[j];
            if(!isfinite(x)||!isfinite(y)||fabs(x-y)>1e-9) return 0;
        }
    }
    return 1;
}
int main(int argc,char **argv) {
    Dwg_Data in={0},check={0}; Snapshot a={0},b={0}; int error,rc=1; FILE *f;
    if(argc!=3) {fprintf(stderr,"Uso: dwgwriter entrada.dxf saida.dwg\n");return 2;}
    f=fopen(argv[2],"rb");if(f){fclose(f);fprintf(stderr,"Saida ja existe.\n");return 1;}
    in.header.version=R_2000;
    error=dxf_read_file(argv[1],&in);
    if(error>=DWG_ERR_CRITICAL){fprintf(stderr,"Falha ao ler DXF: 0x%x\n",error);goto done;}
    a=snapshot(&in);
    if(!a.faces){fprintf(stderr,"DXF sem faces 3D.\n");goto done;}
    in.header.version=R_2000;
    error=dwg_write_file(argv[2],&in);
    if(error){fprintf(stderr,"Falha ao escrever DWG: 0x%x\n",error);remove(argv[2]);goto done;}
    error=dwg_read_file(argv[2],&check);
    if(error>=DWG_ERR_CRITICAL){fprintf(stderr,"Falha ao reler DWG: 0x%x\n",error);remove(argv[2]);goto done;}
    b=snapshot(&check);
    if(!compare(&a,&b)){fprintf(stderr,"Geometria ou unidades alteradas na releitura.\n");remove(argv[2]);goto done;}
    printf("DWG AC1015 validado: %zu faces 3D, %zu circulos; unidades %u; coordenadas conferidas (1e-9).\n",b.faces,b.circles,b.units);
    rc=0;
done:
    free(a.items);free(b.items);dwg_free(&in);dwg_free(&check);return rc;
}
