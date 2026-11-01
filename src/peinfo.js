const fs=require('fs');const b=fs.readFileSync(process.argv[2]);
const pe=b.readUInt32LE(0x3c);const coff=pe+4;const ns=b.readUInt16LE(coff+2);const soh=b.readUInt16LE(coff+16);const opt=coff+20;
console.log('magic',b.readUInt16LE(opt).toString(16),'nsec',ns,'sizeOptHdr',soh);
console.log('ImageBase',b.readBigUInt64LE(opt+24).toString(16),'SectAlign',b.readUInt32LE(opt+32).toString(16),'FileAlign',b.readUInt32LE(opt+36).toString(16),'SizeOfImage',b.readUInt32LE(opt+56).toString(16),'SizeOfHeaders',b.readUInt32LE(opt+60).toString(16),'Checksum',b.readUInt32LE(opt+64).toString(16),'DllChar',b.readUInt16LE(opt+70).toString(16));
const nd=b.readUInt32LE(opt+108);for(let i=0;i<nd;i++){const o=opt+112+i*8;console.log('dir',i,b.readUInt32LE(o).toString(16),b.readUInt32LE(o+4).toString(16));}
const sh=opt+soh;for(let i=0;i<ns;i++){const o=sh+i*40;console.log(b.toString('latin1',o,o+8).replace(/\0/g,''),'va',b.readUInt32LE(o+12).toString(16),'vs',b.readUInt32LE(o+8).toString(16),'raw',b.readUInt32LE(o+20).toString(16),'rs',b.readUInt32LE(o+16).toString(16),'ch',b.readUInt32LE(o+36).toString(16));}
console.log('hdrEnd',(sh+ns*40).toString(16),'fileLen',b.length.toString(16));
