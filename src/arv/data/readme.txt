{title}
{underline}

Disc id:  {id}{part}
Set:      {set}
Burned:   {date}
Contents: {files}, {bytes} bytes (in data/)
Made by:  {software}

中文       这是一张档案光盘。data/ 文件夹里是普通文件，任何电脑都能打开。下面的说明
           是英文的：可以用翻译工具或 AI 助手翻译，命令照原样输入即可。
Español    Este es un disco de archivo. Los archivos de la carpeta data/ son archivos
           normales que cualquier ordenador puede abrir. Las instrucciones de abajo
           están en inglés: una herramienta de traducción o un asistente de IA puede
           traducirlas, y los comandos funcionan tal como están escritos.
हिन्दी       यह एक आर्काइव डिस्क है। data/ फ़ोल्डर में साधारण फ़ाइलें हैं, जिन्हें कोई भी
           कंप्यूटर खोल सकता है। नीचे के निर्देश अंग्रेज़ी में हैं: अनुवाद टूल या AI सहायक
           उनका अनुवाद कर सकता है, और कमांड जैसे लिखे हैं वैसे ही काम करते हैं।
العربية    هذا قرص أرشيف. الملفات في المجلد data/ ملفات عادية يستطيع أي حاسوب فتحها.
           التعليمات أدناه بالإنجليزية: يمكن ترجمتها بأداة ترجمة أو بمساعد ذكاء
           اصطناعي، والأوامر تعمل كما هي مكتوبة.
Français   Ceci est un disque d'archive. Les fichiers du dossier data/ sont des
           fichiers ordinaires, que tout ordinateur peut ouvrir. Les instructions
           ci-dessous sont en anglais : un outil de traduction ou un assistant d'IA
           peut les traduire, et les commandes fonctionnent telles qu'elles sont écrites.
Русский    Это архивный диск. В папке data/ лежат обычные файлы, которые откроет любой
           компьютер. Инструкции ниже на английском: их можно перевести переводчиком
           или ИИ-помощником, а команды работают так, как написаны.
Português  Este é um disco de arquivo. Os arquivos da pasta data/ são arquivos comuns,
           que qualquer computador consegue abrir. As instruções abaixo estão em
           inglês: uma ferramenta de tradução ou um assistente de IA pode traduzi-las,
           e os comandos funcionam exatamente como estão escritos.
日本語     これはアーカイブ用のディスクです。data/ フォルダーのファイルは普通のファイル
           で、どのコンピューターでも開けます。以下の説明は英語です。翻訳ツールや AI
           アシスタントで翻訳でき、コマンドは書かれたとおりに使えます。
Deutsch    Dies ist eine Archiv-Disc. Die Dateien im Ordner data/ sind gewöhnliche
           Dateien, die jeder Computer öffnen kann. Die Anleitung unten ist auf
           Englisch: Ein Übersetzungsprogramm oder ein KI-Assistent kann sie
           übersetzen, und die Befehle funktionieren so, wie sie dastehen.
বাংলা       এটি একটি আর্কাইভ ডিস্ক। data/ ফোল্ডারের ফাইলগুলো সাধারণ ফাইল, যেকোনো কম্পিউটারে
           খোলা যায়। নিচের নির্দেশনা ইংরেজিতে: অনুবাদ টুল বা এআই সহকারী দিয়ে অনুবাদ করা
           যায়, আর কমান্ডগুলো যেমন লেখা আছে ঠিক তেমনই কাজ করে।

{plain}

The rest of this page is for checking the disc and, if it is ever damaged,
repairing it. Anyone comfortable with a command line can follow it.

This disc is a BagIt bag (RFC 8493) with dvdisaster RS03 error correction
data stored after the filesystem.

BROWSE
  Open index.html in any web browser. It lists every file on this disc
  and{other_discs}.

SEARCH
  Catalogue software that reads this format can search every disc (the
  spec: tools/{repo}/docs/spec/smart-archive-format.md).
  From the root of the mounted disc, with the reader (see RESTORE):
    ./arv find PATTERN                 (every disc in this disc's catalogue)
    ./arv list
  Or plain text tools: grep -ri PATTERN catalog/volumes/*/listing.tsv
  Or GNU recutils on the catalogue (below): recsel -t Disc catalog.rec
    (more: tools/{repo}/docs/notes/recutils.md)

VERIFY (detect damage)
  From the root of the mounted disc, any of:
    sha256sum -c manifest-sha256.txt   (any Unix-like system; nothing else)
    shasum -a 256 -c manifest-sha256.txt   (macOS, which has no sha256sum)
  These find a changed file, not a missing or an extra one. For that, this
  prints BYTES.FILES, which must be the Payload-Oxum line in bag-info.txt:
    find data -type f -exec wc -c {{}} + | awk '$2 != "total" {{s += $1; n++}} END {{print s "." n}}'
    ./arv check .                      (arv: see RESTORE; checks it as a BagIt
                                        bag: every manifest, every file, no
                                        file too many or missing)
    python3 tools/{repo}/upstream/bagit-python/bagit.py --validate .
                                       (the Library of Congress's BagIt
                                        validator; tools/{repo}/docs/notes/bagit.md)

RESTORE (copy the files back)
  Copying data/ anywhere is enough for most files. Symbolic links in the
  original folder, execute bits and dates are in the listing,
  catalog/volumes/*/listing.tsv; the reader in tools/ brings them back too,
  checking every file as it copies. {ape_use}Build it with any C compiler:
    cc -O2 -pthread -o arv tools/{repo}/src/arv/*.c tools/{repo}/src/bagit/bagit.c \
      tools/{repo}/src/udfwrite/udfwrite.c tools/{repo}/src/rs03/rs03.c
    ./arv check .                      (or: ./arv restore . ~/restored)
  Without a compiler, after copying data/ to DEST, recreate the links with:
    awk -F'\t' '$3 ~ /^link (recorded|broken)/ {{print $4 "\t" $5}}' \
      catalog/volumes/*/listing.tsv | while IFS="$(printf '\t')" read -r t p
      do ln -s "$t" "DEST/$p"; done

REPAIR (fix damage)
  1. Read the disc into an image, even if parts are unreadable, with either
     of (unread sectors are left as zeros, which the repair finds):
       ddrescue -b 2048 /dev/sr0 disc.iso disc.map       (GNU ddrescue)
       dvdisaster -d /dev/sr0 -r -i disc.iso             (dvdisaster Light,
         https://github.com/teaching-droid/dvdisaster-light: add --rescue)
{size_check}  2. Repair, then check, with the reader in tools/ (arv or arv.com: see
     RESTORE); it needs nothing else:
       ./arv check disc.iso --repair
     or with dvdisaster: dvdisaster -i disc.iso -f{dvdisaster_n}
     If arv cannot repair it, it prints the dvdisaster Light commands to
     paste: dvdisaster looks harder for the error correction's layout{n_note}.
  3. Still damaged? Every copy of this disc is identical. Put in another copy
     and read it into the same image; only the missing sectors are read:
       ddrescue -b 2048 /dev/sr0 disc.iso disc.map       (the same map file)
       dvdisaster -d /dev/sr0 -r -j 1 -i disc.iso
     then repair as in step 2.
  Then burn or mount disc.iso and verify as above.

WITHOUT A UDF READER (cut the files out of the image)
  If nothing can read the disc's file system but its image reads (and
  repairs, above), every file can still be cut out of it: each one is
  stored whole, in one run of 2048-byte sectors. Where each starts is in
  catalog/volumes/{id}/extents.tsv (start sector, size in bytes, path),
  which this disc cannot carry for itself but every later disc of the
  archive does, and so does the home catalogue. For each line:
    dd if=disc.iso bs=2048 skip=START count=$(( (SIZE + 2047) / 2048 )) \
      | head -c SIZE > FILE
  then check FILE against catalog/volumes/{id}/manifest.sha256 (sha256sum).

CATALOGUE
  catalog.rec             this disc's record (GNU recutils format, plain text)
{catalog_lines}
TOOLS
  tools/{repo}/           the program that made this disc; its docs/spec/
                          folder describes the disc format, the UDF profile
                          and the error correction, so other programs can be
                          written to read and repair it
{bundle_line}{ape_line}