/** Local operator screen; all discussion text is assigned with textContent. */
export const PUBLICATION_EDITOR = `<!doctype html><html lang="ja"><meta charset="utf-8">
<title>議論結果の公開設定</title><main><h1>議論結果の公開設定</h1>
<p>公開するのは題名と結論です。内容が変わると再確認するまで非公開になります。</p>
<h2 id="title"></h2><pre id="conclusion" style="white-space:pre-wrap"></pre>
<form><label><input id="publishable" type="checkbox">公開可能</label>
<label>公開範囲 <select id="scope"><option value="private">非公開</option><option value="public">全公開</option><option value="glab">GLab</option></select></label>
<button disabled>保存</button></form><p id="status" role="status"></p></main>
<script>
const id=new URLSearchParams(location.search).get('id');
const endpoint='/api/admin/conclusion-publication';
const status=document.getElementById('status');
const button=document.querySelector('button');
let revision;
async function load(){
 try {
  const stateResponse=await fetch(endpoint+'?id='+encodeURIComponent(id));
  if(!stateResponse.ok)throw Error('公開設定を取得できませんでした');
  const state=await stateResponse.json();
  revision=state.revision;
  document.getElementById('title').textContent=state.title;
  document.getElementById('conclusion').textContent=state.conclusion;
  document.getElementById('publishable').checked=state.publishable;
  document.getElementById('scope').value=state.scope;
  button.disabled=false;
 }catch(error){status.textContent=error.message;}
}
document.querySelector('form').addEventListener('submit',async event=>{
 event.preventDefault(); button.disabled=true;
 try{
  const response=await fetch(endpoint,{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,revision,publishable:document.getElementById('publishable').checked,scope:document.getElementById('scope').value})});
  if(!response.ok)throw Error('保存できませんでした。画面を再読み込みし、結論を確認してください');
  status.textContent='保存しました';
 }catch(error){status.textContent=error.message;}finally{button.disabled=false;}
});
void load();
</script></html>`;
