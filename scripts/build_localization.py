"""Compile the reviewed UTF-8 Markdown dictionary; no client binary modification."""
import json,re,pathlib,sys
root=pathlib.Path(__file__).resolve().parents[1]
source=root/'src/localization/localization.md'
entries={};ids={};errors=[]
def unescape(s):
 return s.replace(r'\|','|').replace(r'\r','\r').replace(r'\n','\n').replace(r'\t','\t').replace(r'\\','\\')
def fmt(s):return re.findall(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hlLwI])?[diuoxXfFeEgGaAcCsSpn]|%)',s)
def add(key,value,ident):
 if not key or not value:errors.append(f'{ident}: empty text');return
 if fmt(key)!=fmt(value):errors.append(f'{ident}: format mismatch {fmt(key)} vs {fmt(value)}');return
 if key in entries and entries[key]!=value:errors.append(f'{ident}: conflicting translation with {ids[key]}: {key!r}');return
 entries[key]=value;ids[key]=ident
for line in source.read_text(encoding='utf-8-sig').splitlines():
 if not re.match(r'\| [A-I]\d+(?:\.\d+)? \|',line):continue
 parts=[unescape(x.strip()) for x in re.split(r'(?<!\\)\|',line)[1:-1]]
 if len(parts)!=5:errors.append('Bad columns: '+line);continue
 ident,kind,jp,zh,note=parts
 if '…' in jp or '（长文' in jp:errors.append(f'{ident}: incomplete source');continue
 if '～' in jp and '(上家)' in jp:
  for old,new in [('上家','上家'),('対面','对家'),('下家','下家'),('自家','自家')]:
   add(jp.split('(')[0]+'('+old+')',zh.split('(')[0]+'('+new+')',ident)
  add(jp.split('(')[0],zh.split('(')[0],ident)
 elif ident in ('F1','F5'):
  add(jp,zh,ident)
  add(jp.split('(')[0],zh.split('(')[0],ident)
 elif ident=='E2':
  for rank in ['初','二','三','四','五','六','七','八','九','十']:add(rank+'段',rank+'段',ident)
 elif ident=='E2.2':
  for n in range(1,10):add(chr(0xff10+n)+'級',chr(0xff10+n)+'级',ident)
 elif ident=='D13.3':
  add(jp,zh,ident);add(' '+jp,' '+zh,ident)
 elif ident=='D71':
  add(jp,zh,ident);add(' '+jp+' ',' '+zh+' ',ident)
  for n in range(10):add(f'{n}本場 ',f'{n}本场 ',ident);add(f' {n}本場',f' {n}本场',ident)
 elif ident in ('C111','C116','C117'):
  add(jp,zh,ident);add(' '+jp,' '+zh,ident)
 else:add(jp,zh,ident)
# Explicit variants requested by table notes.
for wind in '東南西北':
 for n in range(1,5):add(wind+chr(0xff10+n)+'局',wind+chr(0xff10+n)+'局','D14')
 for prefix,zh in [('場風','場風'),('自風','自風')]:add(prefix+' '+wind,zh+' '+wind,'D17/D18')
for n in [2,3]:add('イベント会場'+chr(0xff10+n),'活动会场'+chr(0xff10+n),'B18')
for n in [50,75]:add('縮小'+str(n)+'%','缩小'+str(n)+'%','F27')
for k,v in list(entries.items()):
 if ids[k].startswith('E') and '%' not in k and '\n' not in k and ':' not in k and '/' not in k and '■' not in k:
  add(k+': ',v+': ','E-label')
# Explicit visible UI fragments, not arbitrary word replacements in files/protocol.
extras={
 'キャンセル':'取消','ﾒﾆｭｰ':'菜单','メニュー':'菜单','適用':'应用','ヘルプ':'帮助',
 'を支払います。よろしいですか？':'作为费用支付。确定吗？',
 'を支払います。よろしいですか？\r\n':'作为费用支付。确定吗？\r\n',
 '有料会員限定':'仅限有料会员','無料会員限定':'仅限無料会员','未使用':'未使用',
 '音声':'语音','サウンド':'声音','観戦':'观战','表示':'显示','操作':'操作',
 '牌譜保存':'保存牌谱','入場':'入场','対局開始':'对局开始'}
for k,v in extras.items():
 if k not in entries:add(k,v,'extra')
if errors:
 print('\n'.join(errors));sys.exit(1)
# Longest-first assists composite label replacement; exact mappings stay O(1).
items=sorted(entries.items(),key=lambda kv:(-len(kv[0]),kv[0]))
cpp=lambda x:'L'+json.dumps(x,ensure_ascii=True)
text='// Generated from src/localization/localization.md by scripts/build_localization.py.\n'
text+='static const TranslationEntry kTranslations[]={\n'
text+=''.join('    {'+cpp(k)+','+cpp(v)+'},\n' for k,v in items)+'};\n'
output=root/'src/localization/generated/localization_data.inc'
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text(text,encoding='utf-8')
print(f'Generated {len(items)} validated translation entries')
