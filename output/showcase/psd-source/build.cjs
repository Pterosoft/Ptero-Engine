const fs = require('fs');
const path = require('path');
const { createCanvas, loadImage, GlobalFonts } = require('@napi-rs/canvas');
const { initializeCanvas, writePsdBuffer, readPsd } = require('ag-psd');
initializeCanvas(createCanvas);
GlobalFonts.registerFromPath('C:/Windows/Fonts/arialbd.ttf', 'Showcase Bold');
GlobalFonts.registerFromPath('C:/Windows/Fonts/consola.ttf', 'Showcase Mono');
const out = path.resolve(__dirname, '..');
const W=2400, H=1350;
const composite=createCanvas(W,H), ctx=composite.getContext('2d');
function raster(name,x,y,w,h,draw) {
  const canvas=createCanvas(w,h); draw(canvas.getContext('2d'));
  return {name,left:x,top:y,canvas};
}
function paint(layer) { ctx.drawImage(layer.canvas,layer.left,layer.top); }
function text(name,value,x,baseline,size,color,bold=false,tracking=0) {
  const font=`${size}px "${bold?'Showcase Bold':'Showcase Mono'}"`;
  const probe=createCanvas(1,1).getContext('2d'); probe.font=font;
  const advance=probe.measureText(value).width + (value.length-1)*tracking*size/1000;
  const pad=10, top=Math.floor(baseline-size*1.1);
  const layer=raster(name,x-pad,top,Math.ceil(advance+2*pad+8),Math.ceil(size*1.5),c=>{
    c.font=font;c.fillStyle=color;
    let px=pad;
    if(!tracking)c.fillText(value,px,baseline-top);
    else for(const ch of value){c.fillText(ch,px,baseline-top);px+=c.measureText(ch).width+tracking*size/1000;}
  });
  layer.text={text:value,transform:[1,0,0,1,x,baseline],shapeType:'point',antiAlias:'smooth',
    style:{font:{name:bold?'Arial-BoldMT':'Consolas'},fontSize:size,fillColor:rgb(color),tracking,autoKerning:true},
    paragraphStyle:{justification:'left'}};
  return layer;
}
function rgb(hex){return {r:parseInt(hex.slice(1,3),16),g:parseInt(hex.slice(3,5),16),b:parseInt(hex.slice(5,7),16)};}
async function main(){
  const bg=raster('Background · charcoal',0,0,W,H,c=>{c.fillStyle='#121313';c.fillRect(0,0,W,H)});paint(bg);
  const square=raster('Accent · coral square',86,55,30,30,c=>{c.fillStyle='#F77850';c.fillRect(0,0,30,30)});
  const line=raster('Accent · coral rule',645,69,144,3,c=>{c.fillStyle='#F77850';c.fillRect(0,0,144,3)});
  const brand=text('Brand · editable','PTERO—ENGINE',152,80,29,'#F7F5EC',false,390);
  const index=text('Feature index · editable','FEATURE / 01',2075,80,28,'#D8D8D1',false,100);
  const title=text('Feature name · editable','Virtualized Geometry',82,219,152,'#F7F5EC',true);
  const footerLeft=text('Footer brand · editable','PTERO—ENGINE',86,1288,27,'#A9AAA3',false,170);
  const footerRight=text('Category · editable','RENDERING',2130,1288,27,'#A9AAA3',false,130);
  const photo=await loadImage('K:/Ptero-Engine/Screenshots/screenshot_20261019_203122.png');
  const frame={x:86,y:272,w:2228,h:954,r:15};
  const photoH=Math.round(frame.w*photo.height/photo.width);
  const photoY=frame.y-Math.round((photoH-frame.h)*0.48);
  const screenshot=raster('Engine screenshot · move or replace image',frame.x,photoY,frame.w,photoH,c=>c.drawImage(photo,0,0,frame.w,photoH));
  const mask=createCanvas(frame.w,frame.h), mc=mask.getContext('2d');
  mc.fillStyle='#000000';mc.fillRect(0,0,frame.w,frame.h);mc.fillStyle='#ffffff';mc.beginPath();mc.roundRect(0,0,frame.w,frame.h,frame.r);mc.fill();
  screenshot.mask={left:frame.x,top:frame.y,canvas:mask,defaultColor:0};
  ctx.save();ctx.beginPath();ctx.roundRect(frame.x,frame.y,frame.w,frame.h,frame.r);ctx.clip();paint(screenshot);ctx.restore();
  const border=raster('Frame · subtle outline',frame.x-1,frame.y-1,frame.w+2,frame.h+2,c=>{c.strokeStyle='#555044';c.lineWidth=2;c.beginPath();c.roundRect(1,1,frame.w,frame.h,frame.r);c.stroke()});paint(border);
  for(const layer of [square,line,brand,index,title,footerLeft,footerRight])paint(layer);
  const psd={width:W,height:H,canvas:composite,children:[bg,
    {name:'02 · Screenshot and frame',opened:true,children:[screenshot,border]},
    {name:'01 · Header — edit title and feature number',opened:true,children:[square,line,brand,index,title]},
    {name:'03 · Footer',opened:true,children:[footerLeft,footerRight]}]};
  const target=path.join(out,'virtualized-geometry-editable.psd');
  fs.writeFileSync(target,writePsdBuffer(psd,{generateThumbnail:true}));
  fs.writeFileSync(path.join(out,'virtualized-geometry-editable-preview.png'),composite.toBuffer('image/png'));
  const check=readPsd(fs.readFileSync(target),{skipLayerImageData:true,skipCompositeImageData:true,skipThumbnail:true});
  const texts=check.children.flatMap(l=>l.children||[l]).filter(l=>l.text).map(l=>l.text.text);
  if(texts.length!==5 || !texts.includes('Virtualized Geometry'))throw Error('Editable text verification failed');
  console.log(JSON.stringify({file:target,width:check.width,height:check.height,editableTextLayers:texts,bytes:fs.statSync(target).size}));
}
main().catch(e=>{console.error(e);process.exit(1)});
