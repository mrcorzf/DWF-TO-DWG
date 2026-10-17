/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone LibreDWG frontend. No Autodesk toolkit is linked here.
 */
#include "config.h"
#include "dwg.h"
#include "dwg_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned type; double values[12]; char layer[128]; } Geometry;
typedef struct { Geometry *items; size_t count, faces, circles; unsigned units; } Snapshot;

/* Read the documented ASCII DXF subset emitted by dwfcore. Reconstructing
 * geometry explicitly avoids carrying incomplete imported document metadata
 * and deferred layer references into the new DWG database. */
static int read_geometry(const char *path,Snapshot *s,Dwg_Data *metadata) {
    FILE *f=fopen(path,"rb");char codebuf[64],value[4096],variable[64]="";int entities=0,header=0,section=0,version=0,finished=0;
    Geometry current={0};unsigned fields=0;size_t capacity=0;
    if(!f)return 1;
    while(fgets(codebuf,sizeof(codebuf),f)) {
        char *end;long code=strtol(codebuf,&end,10);double number;
        if(end==codebuf||code<0||code>1071||!fgets(value,sizeof(value),f))goto invalid;
        value[strcspn(value,"\r\n")]=0;
        if(code==0) {
            if(current.type) {
                if((current.type==1&&fields!=0xfff)||(current.type==2&&(fields&15)!=15))goto invalid;
                if(s->count>=2000000)goto invalid;
                if(s->count==capacity) {size_t n=capacity?capacity*2:1024;Geometry *items=(Geometry*)realloc(s->items,n*sizeof(Geometry));if(!items)goto invalid;s->items=items;capacity=n;}
                s->items[s->count++]=current;if(current.type==1)s->faces++;else s->circles++;
                memset(&current,0,sizeof(current));fields=0;
            }
            if(!strcmp(value,"SECTION")){section=1;entities=header=0;continue;}
            if(!strcmp(value,"ENDSEC")){entities=header=0;continue;}
            if(!strcmp(value,"EOF")){finished=1;break;}
            if(entities) {
                if(!strcmp(value,"3DFACE"))current.type=1;
                else if(!strcmp(value,"CIRCLE")){current.type=2;current.values[6]=1;}
                else goto invalid;
                strcpy(current.layer,"0");
            }
            continue;
        }
        if(section&&code==2){entities=!strcmp(value,"ENTITIES");header=!strcmp(value,"HEADER");section=0;continue;}
        if(header) {
            if(code==9){if(strlen(value)>=sizeof(variable))goto invalid;strcpy(variable,value);continue;}
            if(!strcmp(variable,"$ACADVER")&&code==1){version=!strcmp(value,"AC1015");continue;}
            if(!strcmp(variable,"$INSUNITS")&&code==70){long u=strtol(value,&end,10);if(*end||u<1||u>24)goto invalid;s->units=(unsigned)u;}
            if(!strcmp(variable,"$MEASUREMENT")&&code==70)metadata->header_vars.MEASUREMENT=atoi(value);
            if((!strcmp(variable,"$EXTMIN")||!strcmp(variable,"$EXTMAX"))&&(code==10||code==20||code==30)){
                BITCODE_3BD *point=!strcmp(variable,"$EXTMIN")?&metadata->header_vars.EXTMIN:&metadata->header_vars.EXTMAX;
                number=strtod(value,&end);if(*end||!isfinite(number))goto invalid;
                if(code==10)point->x=number;else if(code==20)point->y=number;else point->z=number;
            }
        }
        if(!entities||!current.type)continue;
        if(code==8){if(!*value||strlen(value)>=sizeof(current.layer))goto invalid;strcpy(current.layer,value);continue;}
        if(code==5||code==100||code==330)continue;
        number=strtod(value,&end);if(*end||!isfinite(number))goto invalid;
        if(current.type==1) {
            int coordinate=(int)code/10-1,corner=(int)code%10;
            if(coordinate>=0&&coordinate<3&&corner>=0&&corner<4){current.values[3*corner+coordinate]=number;fields|=1u<<(3*corner+coordinate);}
            else if(code!=70||number!=0)goto invalid;
        } else {
            if(code==10){current.values[0]=number;fields|=1;}
            else if(code==20){current.values[1]=number;fields|=2;}
            else if(code==30){current.values[2]=number;fields|=4;}
            else if(code==40){if(number<=0)goto invalid;current.values[3]=number;fields|=8;}
            else if(code==210)current.values[4]=number;
            else if(code==220)current.values[5]=number;
            else if(code==230)current.values[6]=number;
            else goto invalid;
        }
    }
    fclose(f);if(finished&&version&&s->units&&s->faces)return 0;
    fprintf(stderr,"DXF incompleto, sem unidades ou fora do subconjunto suportado.\n");return 1;
invalid:
    fclose(f);fprintf(stderr,"DXF invalido ou entidade nao suportada.\n");return 1;
}

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
        if(g->type) {
            Dwg_Object *layer=dwg_ref_object_silent(dwg,o->tio.entity->layer);
            const char *name=layer&&layer->fixedtype==DWG_TYPE_LAYER?layer->tio.object->tio.LAYER->name:NULL;
            if(!name||strlen(name)>=sizeof(g->layer)){fprintf(stderr,"Camada ausente ou nome invalido.\n");exit(1);}
            strcpy(g->layer,name);
        }
    }
    return s;
}
static int compare(const Snapshot *a,const Snapshot *b) {
    size_t i,j;
    if(a->count!=b->count||a->faces!=b->faces||a->circles!=b->circles||a->units!=b->units) return 0;
    for(i=0;i<a->count;++i) {
        if(a->items[i].type!=b->items[i].type||strcmp(a->items[i].layer,b->items[i].layer)) return 0;
        for(j=0;j<12;++j) {
            double x=a->items[i].values[j],y=b->items[i].values[j];
            if(!isfinite(x)||!isfinite(y)||fabs(x-y)>1e-9) return 0;
        }
    }
    return 1;
}
static int handle_compare(const void *a,const void *b) {
    BITCODE_HV x=*(const BITCODE_HV*)a,y=*(const BITCODE_HV*)b;return x>y?1:x<y?-1:0;
}
/* Checks independent of the geometry roundtrip. HANDSEED is deliberately
 * excluded from dangling-reference checks: it denotes the next free handle. */
static int structure(Dwg_Data *d) {
    unsigned errors=0,i; BITCODE_HV *handles;
#define CHECK_REF(name,type) do { Dwg_Object *o=dwg_ref_object_silent(d,d->header_vars.name);if(!o||o->fixedtype!=DWG_TYPE_##type){fprintf(stderr,"Estrutura: " #name " ausente/invalido.\n");errors++;}} while(0)
    CHECK_REF(BLOCK_CONTROL_OBJECT,BLOCK_CONTROL);CHECK_REF(LAYER_CONTROL_OBJECT,LAYER_CONTROL);
    CHECK_REF(STYLE_CONTROL_OBJECT,STYLE_CONTROL);CHECK_REF(LTYPE_CONTROL_OBJECT,LTYPE_CONTROL);
    CHECK_REF(VIEW_CONTROL_OBJECT,VIEW_CONTROL);CHECK_REF(UCS_CONTROL_OBJECT,UCS_CONTROL);
    CHECK_REF(VPORT_CONTROL_OBJECT,VPORT_CONTROL);CHECK_REF(APPID_CONTROL_OBJECT,APPID_CONTROL);
    CHECK_REF(DIMSTYLE_CONTROL_OBJECT,DIMSTYLE_CONTROL);CHECK_REF(DICTIONARY_NAMED_OBJECT,DICTIONARY);
    CHECK_REF(BLOCK_RECORD_MSPACE,BLOCK_HEADER);CHECK_REF(BLOCK_RECORD_PSPACE,BLOCK_HEADER);
    CHECK_REF(TEXTSTYLE,STYLE);CHECK_REF(DIMTXSTY,STYLE);CHECK_REF(DIMSTYLE,DIMSTYLE);
    CHECK_REF(CLAYER,LAYER);CHECK_REF(LTYPE_CONTINUOUS,LTYPE);
#undef CHECK_REF
    handles=(BITCODE_HV*)calloc(d->num_objects,sizeof(BITCODE_HV));if(!handles)return 1;
    for(i=0;i<d->num_objects;i++) {
        Dwg_Object *o=&d->object[i];handles[i]=o->handle.value;
        if(!handles[i])errors++;
        if(o->fixedtype==DWG_TYPE__3DFACE||o->fixedtype==DWG_TYPE_CIRCLE) {
            Dwg_Object_Entity *e=o->tio.entity;
            Dwg_Object *layer=dwg_ref_object_silent(d,e->layer);
            if(!layer||layer->fixedtype!=DWG_TYPE_LAYER)errors++;
            if(e->entmode!=2) {Dwg_Object *owner=dwg_ref_object_silent(d,e->ownerhandle);if(!owner||owner->fixedtype!=DWG_TYPE_BLOCK_HEADER)errors++;}
        }
    }
    qsort(handles,d->num_objects,sizeof(*handles),handle_compare);
    for(i=1;i<d->num_objects;i++)if(handles[i]==handles[i-1])errors++;
    free(handles);
    for(i=0;i<d->num_object_refs;i++) {
        Dwg_Object_Ref *r=d->object_ref[i];
        if(r && r!=d->header_vars.HANDSEED && r->absolute_ref && !dwg_ref_object_silent(d,r)) {
            if(errors<25)fprintf(stderr,"Estrutura: referencia sem destino %llu.\n",(unsigned long long)r->absolute_ref);
            errors++;
        }
    }
    if(errors)fprintf(stderr,"Estrutura: %u problema(s).\n",errors);
    return errors!=0;
}
typedef struct { char name[128]; BITCODE_HV handle; } Layer;
static Dwg_Data *document(const Snapshot *source,const Dwg_Data *input) {
    Dwg_Data *d=dwg_new_Document(R_2000,0,0);Dwg_Object *model;Dwg_Object_BLOCK_HEADER *block;
    Layer *layers;size_t count=0,i,j;int error;
    if(!d)return NULL;
    d->header_vars.INSUNITS=source->units;d->header_vars.MEASUREMENT=input->header_vars.MEASUREMENT;
    d->header_vars.EXTMIN=input->header_vars.EXTMIN;d->header_vars.EXTMAX=input->header_vars.EXTMAX;
    {
        Dwg_Object_DIMSTYLE *style=dwg_add_DIMSTYLE(d,"Standard");
        if(!style)goto failed;
        d->header_vars.DIMSTYLE=dwg_add_handleref(d,5,dwg_obj_generic_handlevalue(style),NULL);
    }
    layers=(Layer*)calloc(source->count,sizeof(Layer));if(!layers)goto failed;
    /* Build layers first, then obtain the model-space object; object-array
     * reallocations must never invalidate a stored Dwg_Object pointer. */
    for(i=0;i<source->count;i++) {
        const char *name=source->items[i].layer;
        for(j=0;j<count;j++)if(!strcmp(layers[j].name,name))break;
        if(j==count) {
            Dwg_Object_LAYER *layer;
            strcpy(layers[count].name,name);
            if(!strcmp(name,"0"))layers[count].handle=d->header_vars.CLAYER->absolute_ref;
            else {
                layer=dwg_add_LAYER(d,name);if(!layer){free(layers);goto failed;}
                layer->color.index=7;
                layer->ltype=dwg_add_handleref(d,5,d->header_vars.LTYPE_CONTINUOUS->absolute_ref,NULL);
                layers[count].handle=dwg_obj_generic_handlevalue(layer);
            }
            count++;
        }
    }
    model=dwg_model_space_object(d);if(!model){free(layers);goto failed;}
    block=model->tio.object->tio.BLOCK_HEADER;
    for(i=0;i<source->count;i++) {
        const Geometry *g=&source->items[i];Dwg_Object_Entity *entity=NULL;Dwg_Object *object;
        if(g->type==1) {
            dwg_point_3d p[4];Dwg_Entity__3DFACE *face;
            for(j=0;j<4;j++){p[j].x=g->values[j*3];p[j].y=g->values[j*3+1];p[j].z=g->values[j*3+2];}
            face=dwg_add_3DFACE(block,&p[0],&p[1],&p[2],&p[3]);if(face)entity=face->parent;
        } else if(g->type==2) {
            dwg_point_3d center={g->values[0],g->values[1],g->values[2]};Dwg_Entity_CIRCLE *circle=dwg_add_CIRCLE(block,&center,g->values[3]);
            if(circle){circle->extrusion.x=g->values[4];circle->extrusion.y=g->values[5];circle->extrusion.z=g->values[6];entity=circle->parent;}
        }
        if(!entity){free(layers);goto failed;}
        object=&d->object[entity->objid];
        for(j=0;j<count;j++)if(!strcmp(layers[j].name,g->layer))break;
        entity->layer=dwg_add_handleref(d,5,layers[j].handle,object);
    }
    free(layers);error=structure(d);if(error)goto failed;return d;
failed:
    dwg_free(d);free(d);return NULL;
}
int main(int argc,char **argv) {
    Dwg_Data in={0},check={0},*clean=NULL; Snapshot a={0},b={0}; int error,rc=1; FILE *f;
    if(argc==3&&!strcmp(argv[1],"--inspect")) {error=dwg_read_file(argv[2],&check);rc=error>=DWG_ERR_CRITICAL?1:structure(&check);dwg_free(&check);return rc;}
    if(argc!=3) {fprintf(stderr,"Uso: dwgwriter entrada.dxf saida.dwg\n");return 2;}
    f=fopen(argv[2],"rb");if(f){fclose(f);fprintf(stderr,"Saida ja existe.\n");return 1;}
    error=read_geometry(argv[1],&a,&in);
    if(error)goto done;
    if(!a.faces){fprintf(stderr,"DXF sem faces 3D.\n");goto done;}
    clean=document(&a,&in);
    if(!clean){fprintf(stderr,"Falha ao construir documento DWG completo.\n");goto done;}
    error=dwg_write_file(argv[2],clean);
    if(error){fprintf(stderr,"Falha ao escrever DWG: 0x%x\n",error);remove(argv[2]);goto done;}
    error=dwg_read_file(argv[2],&check);
    if(error>=DWG_ERR_CRITICAL){fprintf(stderr,"Falha ao reler DWG: 0x%x\n",error);remove(argv[2]);goto done;}
    if(structure(&check)){remove(argv[2]);goto done;}
    b=snapshot(&check);
    if(!compare(&a,&b)){fprintf(stderr,"Geometria ou unidades alteradas na releitura.\n");remove(argv[2]);goto done;}
    printf("DWG AC1015: %zu faces 3D, %zu circulos; unidades %u; geometria/camadas conferidas (1e-9); referencias estruturais verificadas. Validacao no CAD ainda necessaria.\n",b.faces,b.circles,b.units);
    rc=0;
done:
    free(a.items);free(b.items);dwg_free(&in);dwg_free(&check);if(clean){dwg_free(clean);free(clean);}return rc;
}
