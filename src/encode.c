/*
    Daala video codec
    Copyright (C) 2006-2010 Daala project contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/


#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "encint.h"
#include "generic_code.h"
#include "filter.h"
#include "dct.h"
#include "pvq_code.h"
#include "pvq.h"
#include "intra.h"

static int od_enc_init(od_enc_ctx *_enc,const daala_info *_info){
  int ret;
  ret=od_state_init(&_enc->state,_info);
  if(ret<0)return ret;
  oggbyte_writeinit(&_enc->obb);
  od_ec_enc_init(&_enc->ec,65025);
  _enc->packet_state=OD_PACKET_INFO_HDR;
  _enc->mvest=od_mv_est_alloc(_enc);
  return 0;
}

static void od_enc_clear(od_enc_ctx *_enc){
  od_mv_est_free(_enc->mvest);
  od_ec_enc_clear(&_enc->ec);
  oggbyte_writeclear(&_enc->obb);
  od_state_clear(&_enc->state);
}

daala_enc_ctx *daala_encode_alloc(const daala_info *_info){
  od_enc_ctx *enc;
  if(_info==NULL)return NULL;
  enc=(od_enc_ctx *)_ogg_malloc(sizeof(*enc));
  if(od_enc_init(enc,_info)<0){
    _ogg_free(enc);
    return NULL;
  }
  return enc;
}

void daala_encode_free(daala_enc_ctx *_enc){
  if(_enc!=NULL){
    od_enc_clear(_enc);
    _ogg_free(_enc);
  }
}

int daala_encode_ctl(daala_enc_ctx *_enc,int _req,void *_buf,size_t _buf_sz){
  switch(_req){
    default:return OD_EIMPL;
  }
}

void od_state_mc_predict(od_state *_state,int _ref){
  unsigned char  __attribute__((aligned(16))) buf[16][16];
  od_img        *img;
  int            nhmvbs;
  int            nvmvbs;
  int            pli;
  int            vx;
  int            vy;
  nhmvbs=_state->nhmbs+1<<2;
  nvmvbs=_state->nvmbs+1<<2;
  img=&_state->rec_img;
  for(vy=0;vy<nvmvbs;vy+=4){
    for(vx=0;vx<nhmvbs;vx+=4){
      for(pli=0;pli<img->nplanes;pli++){
        od_img_plane  *iplane;
        unsigned char *p;
        int            blk_w;
        int            blk_h;
        int            blk_x;
        int            blk_y;
        int            y;
        od_state_pred_block(_state,buf[0],sizeof(buf[0]),_ref,pli,vx,vy,2);
        /*Copy the predictor into the image, with clipping.*/
        iplane=img->planes+pli;
        blk_w=16>>iplane->xdec;
        blk_h=16>>iplane->ydec;
        blk_x=vx-2<<2-iplane->xdec;
        blk_y=vy-2<<2-iplane->ydec;
        p=buf[0];
        if(blk_x<0){
          blk_w+=blk_x;
          p-=blk_x;
          blk_x=0;
        }
        if(blk_y<0){
          blk_h+=blk_y;
          p-=blk_y*sizeof(buf[0]);
          blk_y=0;
        }
        if(blk_x+blk_w>img->width>>iplane->xdec){
          blk_w=(img->width>>iplane->xdec)-blk_x;
        }
        if(blk_y+blk_h>img->height>>iplane->ydec){
          blk_h=(img->height>>iplane->ydec)-blk_y;
        }
        for(y=blk_y;y<blk_y+blk_h;y++){
          memcpy(iplane->data+y*iplane->ystride+blk_x,p,blk_w);
          p+=sizeof(buf[0]);
        }
      }
    }
  }
}

#if 0
/*The true forward 4-point type-II DCT basis, to 32-digit (100 bit) precision.
  The inverse is merely the transpose.*/
static const double DCT4_BASIS[4][4]={
  {
     0.5,                                 0.5,
     0.5,                                 0.5
  },
  {
     0.65328148243818826392832158671359,  0.27059805007309849219986160268319,
    -0.27059805007309849219986160268319, -0.65328148243818826392832158671359
  },
  {
     0.5,                                -0.5,
    -0.5,                                 0.5
  },
  {
     0.27059805007309849219986160268319, -0.65328148243818826392832158671359,
     0.65328148243818826392832158671359, -0.27059805007309849219986160268319
  },
};

void idct4(od_coeff _x[],const od_coeff _y[]){
  double t[8];
  int    i;
  int    j;
  for(j=0;j<4;j++){
    t[j]=0;
    for(i=0;i<4;i++)t[j]+=DCT4_BASIS[i][j]*_y[i];
  }
  for(j=0;j<4;j++)_x[j]=t[j];
}

void fdct4(od_coeff _x[],const od_coeff _y[]){
  double t[8];
  int    i;
  int    j;
  for(j=0;j<4;j++){
    t[j]=0;
    for(i=0;i<4;i++)t[j]+=DCT4_BASIS[j][i]*_y[i];
  }
  for(j=0;j<4;j++)_x[j]=t[j];
}
#endif

int daala_encode_img_in(daala_enc_ctx *_enc,od_img *_img,int _duration){
  GenericEncoder model_dc;
  GenericEncoder model_g;
  GenericEncoder model_k;
  int refi;
  int pli;
  int scale;
  if(_enc==NULL||_img==NULL)return OD_EFAULT;
  if(_enc->packet_state==OD_PACKET_DONE)return OD_EINVAL;
  /*Check the input image dimensions to make sure they match the declared video
     size.*/
  if(_img->width!=_enc->state.info.frame_width||
   _img->height!=_enc->state.info.frame_height||
   _img->nplanes!=_enc->state.info.ncomps){
    return OD_EINVAL;
  }
  for(pli=0;pli<_img->nplanes;pli++){
    if(_img->planes[pli].xdec!=_enc->state.info.comps[pli].xdec||
     _img->planes[pli].ydec!=_enc->state.info.comps[pli].ydec){
      return OD_EINVAL;
    }
  }
  /*Init entropy coder*/
  od_ec_enc_reset(&_enc->ec);
  /*Update buffer state.*/
  if(_enc->state.ref_imgi[OD_FRAME_SELF]>=0){
    _enc->state.ref_imgi[OD_FRAME_PREV]=
     _enc->state.ref_imgi[OD_FRAME_SELF];
    /*TODO: Update golden frame.*/
    if(_enc->state.ref_imgi[OD_FRAME_GOLD]<0){
      _enc->state.ref_imgi[OD_FRAME_GOLD]=_enc->state.ref_imgi[OD_FRAME_SELF];
      /*TODO: Mark keyframe timebase.*/
    }
  }
  /*Select a free buffer to use for this reference frame.*/
  for(refi=0;refi==_enc->state.ref_imgi[OD_FRAME_GOLD]||
   refi==_enc->state.ref_imgi[OD_FRAME_PREV]||
   refi==_enc->state.ref_imgi[OD_FRAME_NEXT];refi++);
  _enc->state.ref_imgi[OD_FRAME_SELF]=refi;
  memcpy(&_enc->state.input,_img,sizeof(_enc->state.input));
  /*TODO: Incrment frame count.*/
  if(_enc->state.ref_imgi[OD_FRAME_PREV]>=0/*&&
   daala_granule_basetime(_enc,_enc->state.cur_time)>=19*/){
#if defined(OD_DUMP_IMAGES)&&defined(OD_ANIMATE)
    _enc->state.ani_iter=0;
#endif
    fprintf(stderr,"Predicting frame %i:\n",
     (int)daala_granule_basetime(_enc,_enc->state.cur_time));
#if 0
    od_mv_est(_enc->mvest,OD_FRAME_PREV,452/*118*/);
#endif
    od_state_mc_predict(&_enc->state,OD_FRAME_PREV);
#if defined(OD_DUMP_IMAGES)
    /*Dump reconstructed frame.*/
/*    od_state_dump_img(&_enc->state,&_enc->state.rec_img,"rec");*/
    od_state_fill_vis(&_enc->state);
    od_state_dump_img(&_enc->state,&_enc->state.vis_img,"vis");
#endif
  }
  scale=32;/*atoi(getenv("QUANT"));*/
  generic_model_init(&model_dc);
  generic_model_init(&model_g);
  generic_model_init(&model_k);
  /*TODO: Encode image.*/
  for(pli=0;pli<_img->nplanes;pli++){
    ogg_uint16_t mode_p0[OD_INTRA_NMODES];
    ogg_int64_t  mc_sqerr;
    ogg_int64_t  enc_sqerr;
    ogg_uint32_t npixels;
    od_coeff     *ctmp;
    char         *modes;
    int          i;
    int          x;
    int          y;
    int          w;
    int          h;
#ifdef OD_DPCM
    int          err_accum = 0;
#endif
    int ex_dc = pli>0?8:32768;
    int ex_g = 8;
    int ex_k = 8;
    int anum = 650*4;
    int aden = 256*4;
    int au = 30<<4;
    /*TODO: Use picture dimensions, not frame dimensions.*/
    w=_img->width>>_img->planes[pli].xdec;
    h=_img->height>>_img->planes[pli].ydec;
    mc_sqerr=0;
    enc_sqerr=0;
    npixels=w*h;
    ctmp=calloc((w+15>>4<<4)*(h+15>>4<<4),sizeof(od_coeff));
    modes=calloc((w+15>>2)*(h+15>>2),sizeof(char));
    for(i=0;i<OD_INTRA_NMODES;i++)mode_p0[i]=32768/OD_INTRA_NMODES;
    for(y=0;y<h;y++){
      for(x=0;x<w;x++){
        ctmp[y*w+x]=(*(_img->planes[pli].data+_img->planes[pli].ystride*y+_img->planes[pli].xstride*x)-128);
      }
    }
#if 1
    for(y=2;y<h-2;y+=4){
      for(x=0;x<_img->width>>_img->planes[pli].xdec;x++){
        int j;
        od_coeff p[4];
        for(j=0;j<4;j++){
          p[j]=ctmp[(y+j)*w+x];
        }
        od_pre_filter4(p,p);
        for(j=0;j<4;j++){
          ctmp[(y+j)*w+x]=p[j];
        }
      }
    }
    for(y=0;y<h;y++){
      for(x=2;x<(_img->width>>_img->planes[pli].xdec)-2;x+=4){
        od_pre_filter4(&ctmp[y*w+x],&ctmp[y*w+x]);
      }
    }
#endif

    /*FDCT 4x4 blocks*/
    for(y=0;y<h;y+=4){
      for(x=0;x<(_img->width>>_img->planes[pli].xdec);x+=4){
        od_coeff pred[4*4];
        od_coeff predt[4*4];
        ogg_int16_t pvq_scale[4*4];
        int sgn;
        int qg;
        int cblock[16];
        int j;
        int vk;
        vk=0;
        for(j=0;j<4;j++)od_bin_fdct4(&ctmp[(y+j)*w+x],&ctmp[(y+j)*w+x]);
        for(j=0;j<4;j++){
          od_coeff p[4];
          int k;
          for(k=0;k<4;k++)p[k]=ctmp[(y+k)*w+x+j];
          od_bin_fdct4(p,p);
          for(k=0;k<4;k++)ctmp[(y+k)*w+x+j]=p[k];
        }
        for(j=0;j<16;j++)pvq_scale[j]=0;
        if(x>0&&y>0){
          ogg_uint16_t mode_cdf[OD_INTRA_NMODES];
          od_coeff mode_dist[OD_INTRA_NMODES];
          int m_l, m_ul, m_u, mode;
          m_l  = modes[(y>>2)*(w>>2)+((x>>2)-1)];
          m_ul = modes[((y>>2)-1)*(w>>2)+((x>>2)-1)];
          m_u = modes[((y>>2)-1)*(w>>2)+(x>>2)];
          od_intra_pred_cdf(mode_cdf,OD_INTRA_PRED_PROB_4x4[pli],mode_p0,m_l,m_ul,m_u);
          od_intra_pred4x4_dist(mode_dist,&ctmp[y*w+x],w,pli);
          /*Lambda = 1*/
          mode=od_intra_pred_search(mode_p0,mode_cdf,mode_dist,128,m_l,m_ul,m_u);
          od_intra_pred4x4_get(pred,&ctmp[y*w+x],w,mode);
          od_ec_encode_cdf_unscaled(&_enc->ec,mode,mode_cdf,OD_INTRA_NMODES);
          modes[(y>>2)*(w>>2)+(x>>2)]=mode;
        }else{
          for(j=0;j<16;j++)pred[j]=0;
          if(x>0)pred[0]=ctmp[y*w+x-4];
          else if(y>0)pred[0]=ctmp[(y-4)*w+x];
          else pred[0]=0;
          modes[(y>>2)*(w>>2)+(x>>2)]=0;
        }
        /*Quantize*/
        for(j=0;j<4;j++){
          int k;
          for(k=0;k<4;k++){
            ctmp[(y+k)*w+x+j]=cblock[od_zig4[k*4+j]]=ctmp[(y+k)*w+x+j];/*/scale;*//*OD_DIV_ROUND((p[k]-pred[1]),scale);*/
            predt[od_zig4[k*4+j]]=pred[k*4+j];
          }
        }
        sgn=(cblock[0]-predt[0])<0;
        cblock[0]=floor(pow(fabs(cblock[0]-predt[0])/(scale/2.),3/4.));
        generic_encode(&_enc->ec,&model_dc,cblock[0],&ex_dc,0);
        if(cblock[0])od_ec_enc_bits(&_enc->ec,sgn,1);
        quant_pvq(&cblock[1],&predt[1],pvq_scale,&pred[1],15,scale,&qg);
        generic_encode(&_enc->ec,&model_g,abs(qg),&ex_g,0);
        if(qg)od_ec_enc_bits(&_enc->ec,qg<0,1);
        vk=0;
        for(j=0;j<15;j++)vk+=abs(pred[j+1]);
        generic_encode(&_enc->ec,&model_k,vk,&ex_k,0);
        cblock[0]=pow(cblock[0],4/3.)*(scale/2.);
        cblock[0]*=sgn?-1:1;
        cblock[0]+=predt[0];
        pvq_encoder(&_enc->ec,&pred[1],15,vk,&anum,&aden,&au);
        /*Dequantize*/
        for(j=0;j<4;j++){
          int k;
          for(k=0;k<4;k++){
            ctmp[(y+k)*w+x+j]=cblock[od_zig4[k*4+j]];
          }
        }
      }
    }
    /*iDCT 4x4 blocks*/
    for(y=0;y<h;y+=4){
      for(x=0;x<(_img->width>>_img->planes[pli].xdec);x+=4){
        int j;
        for(j=0;j<4;j++){
          od_coeff p[4];
          int k;
          for(k=0;k<4;k++)p[k]=ctmp[(y+k)*w+x+j];
          od_bin_idct4(p,p);
          for(k=0;k<4;k++)ctmp[(y+k)*w+x+j]=p[k];
        }
        for(j=0;j<4;j++)od_bin_idct4(&ctmp[(y+j)*w+x],&ctmp[(y+j)*w+x]);
      }
    }

#if 1
    for(y=0;y<h;y++){
      for(x=2;x<(_img->width>>_img->planes[pli].xdec)-2;x+=4){
        od_post_filter4(&ctmp[y*w+x],&ctmp[y*w+x]);
      }
    }
    for(y=2;y<h-2;y+=4){
      for(x=0;x<_img->width>>_img->planes[pli].xdec;x++){
        int j;
        od_coeff p[4];
        for(j=0;j<4;j++)p[j]=ctmp[(y+j)*w+x];
        od_post_filter4(p,p);
        for(j=0;j<4;j++)ctmp[(y+j)*w+x]=p[j];
      }
    }
#endif
    for(y=0;y<h;y++)for(x=0;x<_img->width>>_img->planes[pli].xdec;x++){
       unsigned char *recimg;
       recimg=_enc->state.rec_img.planes[pli].data+_enc->state.rec_img.planes[pli].ystride*y+x;
       *recimg=OD_CLAMP255(ctmp[y*w+x]+128);
    }
    free(ctmp);
    free(modes);
    for(y=0;y<h;y++){
      unsigned char *prev_rec_row;
      unsigned char *rec_row;
      unsigned char *inp_row;
      rec_row=_enc->state.rec_img.planes[pli].data+
       _enc->state.rec_img.planes[pli].ystride*y;
      prev_rec_row=rec_row-_enc->state.rec_img.planes[pli].ystride;
      inp_row=_img->planes[pli].data+_img->planes[pli].ystride*y;
      memcpy(_enc->state.ref_line_buf[1],rec_row,w);
      for(x=0;x<_img->width>>_img->planes[pli].xdec;x++){
        int rec_val;
        int inp_val;
        int diff;
        rec_val=rec_row[x];
        inp_val=*(inp_row+_img->planes[pli].xstride*x);
        diff=inp_val-rec_val;
        mc_sqerr+=diff*diff;
#ifdef OD_DPCM
        {
          int pred_diff;
          int qdiff;
          /*DPCM code the residual with uniform quantization.
            This provides simulated residual coding errors, without introducing
             blocking artifacts.*/
          if(x>0)pred_diff=rec_row[x-1]-_enc->state.ref_line_buf[1][x-1];
          else pred_diff=0;
          if(y>0){
            if(x>0){
              pred_diff+=prev_rec_row[x-1]-_enc->state.ref_line_buf[0][x-1];
            }
            pred_diff+=prev_rec_row[x]-_enc->state.ref_line_buf[0][x];
            if(x+1<w){
              pred_diff+=prev_rec_row[x+1]-_enc->state.ref_line_buf[0][x+1];
            }
          }
          pred_diff=OD_DIV_ROUND_POW2(pred_diff,2,2);
          qdiff=(((diff-pred_diff)+(diff-pred_diff>>31)+(5+err_accum))/10)*10+
           pred_diff;
          /*qdiff=(OD_DIV_ROUND_POW2(diff-pred_diff,3,4+err_accum)<<3)+
           pred_diff;*/
          /*fprintf(stderr,"d-p_d: %3i  e_a: %3i  qd-p_d: %3i  e_a: %i\n",
           diff-pred_diff,err_accum,qdiff-pred_diff,diff-qdiff);*/
          err_accum+=diff-qdiff;
          rec_row[x]=OD_CLAMP255(rec_val+qdiff);
        }
#else
/*        rec_row[x]=inp_val;*/
#endif
        diff=inp_val-rec_row[x];
        enc_sqerr+=diff*diff;
      }
      prev_rec_row=_enc->state.ref_line_buf[0];
      _enc->state.ref_line_buf[0]=_enc->state.ref_line_buf[1];
      _enc->state.ref_line_buf[1]=prev_rec_row;
    }
    /*printf("Bytes: %d  ex_dc: %d ex_g: %d ex_k: %d\n",(od_ec_enc_tell(&_enc->ec)+7)>>3,ex_dc,ex_g,ex_k);*/
    if(_enc->state.ref_imgi[OD_FRAME_PREV]>=0){
      fprintf(stderr,
       "Plane %i, Squared Error: %12lli  Pixels: %6u  PSNR:  %5.2f\n",
       pli,(long long)mc_sqerr,npixels,10*log10(255*255.0*npixels/mc_sqerr));
    }
    fprintf(stderr,
     "Encoded Plane %i, Squared Error: %12lli  Pixels: %6u  PSNR:  %5.2f\n",
     pli,(long long)enc_sqerr,npixels,10*log10(255*255.0*npixels/enc_sqerr));
  }

  /*Dump YUV*/
  od_state_dump_yuv(&_enc->state,&_enc->state.rec_img,"out");
  _enc->packet_state=OD_PACKET_READY;
  od_state_upsample8(&_enc->state,
   _enc->state.ref_imgs+_enc->state.ref_imgi[OD_FRAME_SELF],
   &_enc->state.rec_img);
#if defined(OD_DUMP_IMAGES)
  /*Dump reference frame.*/
  /*od_state_dump_img(&_enc->state,
   _enc->state.ref_img+_enc->state.ref_imigi[OD_FRAME_SELF],"ref");*/
#endif
  if(_enc->state.info.frame_duration==0)_enc->state.cur_time+=_duration;
  else _enc->state.cur_time+=_enc->state.info.frame_duration;
  return 0;
}

int daala_encode_packet_out(daala_enc_ctx *_enc,int _last,ogg_packet *_op){
  ogg_uint32_t nbytes;
  if(_enc==NULL||_op==NULL)return OD_EFAULT;
  else if(_enc->packet_state<=0||_enc->packet_state==OD_PACKET_DONE)return 0;
  _op->packet=od_ec_enc_done(&_enc->ec,&nbytes);
  _op->bytes=nbytes;
  fprintf(stderr,"Output Bytes: %ld\n",_op->bytes);
  _op->b_o_s=0;
  _op->e_o_s=_last;
  _op->packetno=0;
  _op->granulepos=_enc->state.cur_time;
  if(_last)_enc->packet_state=OD_PACKET_DONE;
  else _enc->packet_state=OD_PACKET_EMPTY;
  return 1;
}
