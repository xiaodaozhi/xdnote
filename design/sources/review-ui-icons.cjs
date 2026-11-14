/** Reproducible board/previous/current comparison and vector clipping check. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const sharp = require('sharp');
const ROOT = path.resolve(__dirname, '../..');
const iconPaths = require('./ui-icon-paths.cjs');
const esc = s => String(s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('"','&quot;');
async function main() {
  const out=path.resolve(ROOT,process.argv[2]||'build/cycle-139-icon-system');
  if(!out.startsWith(path.join(ROOT,'build')+path.sep))throw Error('Review output must be in this project build/');
  await fs.mkdir(out,{recursive:true});
  const reference=JSON.parse(await fs.readFile(path.join(__dirname,'ui-icon-reference.json'),'utf8'));
  const geometry=JSON.parse(await fs.readFile(path.join(ROOT,'docs/visual/asset-geometry.json'),'utf8')).icon;
  const overlays=[];
  const focus=process.argv.includes('--focus');
  const selected=['settings','document','edit','bold','link','checklist','quote','select-all','window-new','check-circle','error-message','exit'];
  const entries=reference.icons.filter(x=>x.basis==='direct-board-reference'&&(!focus||selected.includes(x.id)));
  const comparison=focus?'icon-comparison-focus.png':'icon-comparison.png';
  const cols=3,cardW=400,cardH=156,head=88;
  let labels='<rect width="1200" height="100%" fill="#f4f5f7"/>';
  const text=(x,y,s,size=13)=>'<text x="'+x+'" y="'+y+'" fill="#1f2937" font-size="'+size+'" font-family="Microsoft YaHei UI, sans-serif">'+esc(s)+'</text>';
  labels+=text(24,32,'xdnote · 图标原稿与重制对照',24)+text(24,58,'原稿局部保留上下文；旧／新 SVG 同为 16 逻辑画布。放大用于看轮廓，不据位图推定逻辑尺寸。');
  for(const [i,item] of entries.entries()){
    const x=(i%cols)*cardW,y=head+Math.floor(i/cols)*cardH;
    labels+='<rect x="'+(x+8)+'" y="'+y+'" width="384" height="146" rx="6" fill="white"/>'+text(x+20,y+22,item.label+' · '+item.id);
    const r=reference.references[item.reference];
    const crop=await sharp(path.join(ROOT,'design',r.sourcePath)).extract(r.pixelRect).resize(60,60,{fit:'contain',background:'#fff'}).png().toBuffer();
    overlays.push({input:crop,left:x+38,top:y+43});labels+=text(x+36,y+126,'原稿局部');
    const current=await fs.readFile(path.join(ROOT,'design/assets/ui-v2/icons/svg',item.id+'.svg'),'utf8');
    const previousRole={exit:'focus-exit','error-message':'error','warning-filled':'warning'}[item.id];
    for(const [col,where,title] of [[1,path.join(out,'before/icons',(previousRole||item.id)+'.svg'),previousRole?'原用 '+previousRole:'原有'],[2,null,'重制']]){
      const source=where?await fs.readFile(where,'utf8').catch(()=>null):current;
      if(source===null){labels+=text(x+40+col*112,y+83,'无旧素材');continue;}
      overlays.push({input:await sharp(Buffer.from(source.replaceAll('currentColor','#1f2937'))).resize(48,48).png().toBuffer(),left:x+44+col*112,top:y+49});labels+=text(x+54+col*112,y+126,title);
    }
  }
  const sheet=Buffer.from('<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="'+(head+Math.ceil(entries.length/cols)*cardH)+'">'+labels+'</svg>');
  await sharp(sheet).composite(overlays).png().toFile(path.join(out,comparison));
  const measures=await checkIconGeometry();
  await fs.writeFile(path.join(out,'icon-geometry.json'),JSON.stringify({kind:'controlled SVG raster; not desktop acceptance',logicalCanvas:16,stroke:geometry.strokeWidth,icons:measures},null,2)+'\n');
  console.log(JSON.stringify({icons:measures.length,shownReferences:entries.length,clipped:0,comparison:path.relative(ROOT,path.join(out,comparison))}));
}
async function checkIconGeometry(){
  const measures=[];
  for(const [id] of Object.entries(iconPaths)){
    const source=await fs.readFile(path.join(ROOT,'design/assets/ui-v2/icons/svg',id+'.svg'),'utf8');
    // Render the full paths into a padded viewBox to detect geometry that a
    // normal 16-unit export would silently clip (including round line caps).
    const padded=source.replace('width="16" height="16" viewBox="0 0 16 16"','width="80" height="80" viewBox="-2 -2 20 20"').replaceAll('currentColor','#1f2937');
    const {data,info}=await sharp(Buffer.from(padded)).ensureAlpha().raw().toBuffer({resolveWithObject:true});
    let outside=0,left=80,top=80,right=-1,bottom=-1;
    for(let y=0;y<info.height;y++)for(let x=0;x<info.width;x++)if(data[(y*info.width+x)*4+3]>32){
      if(x<8||y<8||x>=72||y>=72)outside++;
      left=Math.min(left,x);top=Math.min(top,y);right=Math.max(right,x);bottom=Math.max(bottom,y);
    }
    if(outside||right<left)throw Error(id+': clipped or empty geometry ('+outside+')');
    measures.push({id,inkBounds:{left:(left-8)/4,top:(top-8)/4,width:(right-left+1)/4,height:(bottom-top+1)/4},outsidePixels:outside});
  }
  return measures;
}
module.exports={checkIconGeometry};
if(require.main===module)main().catch(e=>{console.error(e);process.exitCode=1;});
