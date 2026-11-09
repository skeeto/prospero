// PDF image tests: CCITT fax against libtiff's encoder and a real scan,
// samples to RGBA in each colour space and depth, Decode, stencils, soft
// masks, short data and box averages, memory that follows the output,
// JPEG pass-through and conversion, pngfit, and sweeps over cut and
// corrupted fax data

// CCITT fixtures: the pattern ccpattern draws, 2700 by 24, written as an
// uncompressed bilevel TIFF (MinIsWhite, 1 is black, one strip), then
// compressed by libtiff, whose strips these are, and the scan-ccitt
// corpus case's image stream (object 7), as tests/ccittfixtures.py
// makes them all again:
//   $ tiffcp -c g4 -r 24 src.tif g4.tif
//   $ tiffcp -c g3:1d -r 24 src.tif g31d.tif        (and g3:1d:fill)
//   $ tiffcp -c g3:2d -r 24 src.tif g32d.tif        (and g3:2d:fill)
//   $ qpdf --show-object=7 --raw-stream-data scan-ccitt.pdf >scan.bin
// libtiff codes 0 bits as white runs, so with BlackIs1 the rows decode
// to the TIFF's own bits. G3 strips begin every row with an EOL; with
// fill, each EOL ends on a byte (T4Options 4).
static char const ccg4[] =  // 763 bytes
    "3e33e33e33e33e33e33e33e33e33e33e32341aa4583548b06a9160d522c1aa458354b344"
    "80e604f49249249249249248a40d522c1aa4583548b06a9160d522c1aa59a0c0e80a2924"
    "92492492492491f4750d522c1aa4583548b06a9160d522c1aa59a181d014e92492492492"
    "49249292e1aa4583548b06a9160d522c1aa458354b316c0ea09060c183060c183060c183"
    "060c183060c183060c4d41aa4583548b06a9160d522c1aa458354b31540ea1a726a03e19"
    "01c80fc880dcd8cf8cf8cf8cf8cf8cf8cf8cf8cf8cf8cfe4206a9160d522c1aa4583548b"
    "06a9160d52cc2e0760cf060c183060c183060c183060c183060c183060c4a70d522c1aa4"
    "583548b06a9160d522c1aa5985c0ec19b492492492492492488d06a9160d522c1aa45835"
    "48b06a9160d52cc381b0341d9a01b034524924924924924923e88c0d522c1aa4583548b0"
    "6a9160d522c1aa1103506790207492492492492492494ab0d522c1aa4583548b06a9160d"
    "522c1aa110350385e0383060c183060c183060c183060c183060c1830624406a9160d522"
    "c1aa4583548b06a9160d5841e92492492492492491061aa4583548b06a9160d522c1aa45"
    "835640d40a2469a49249249249249247d12e1aa4583548b06a9160d522c1aa4583543206"
    "c72140a924924924924924929a83548b06a9160d522c1aa4583548b06a966080360669d9"
    "501b03360c183060c183060c183060c183060c183060c189061aa4583548b06a9160d522"
    "c1aa458354b31381d832e92492492492492491501aa4583548b06a9160d522c1aa458354"
    "b30080ec18a49249249249249247d1081aa4583548b06a9160d522c1aa458354b300c0ec"
    "a9249249249249249294e1aa4583548b06a9160d522c1aa458354b30b00ea0cb04222222"
    "22222222222240a605cd4f0cf8cf8cf8cf8cf8cf8cf8cf8cf8cf8c930d522c1aa4583548"
    "b06a9160d522c1aa59a6a0753a49249249249249247d1561aa4583548b06a9160d522c1a"
    "a458354b37c0e81b7492492492492492494880d522c1aa4583548b06a9160d522c1aa59a"
    "4c0e8188008008";
static char const ccg31d[] =  // 795 bytes
    "001f1f8fc7e3f1f8fc7e3f1f8fc6341aa2c1aa2c1aa2c1aa2c1aa2c1aacd12039813b715"
    "001e1f8fc7e3f1f8fc7e3f1f8fc69035458354583545835458354583559a0c0e80a1b880"
    "01c1f8fc7e3f1f8fc7e3f1f8fc7eb50d5160d5160d5160d5160d5160d566860740536fc0"
    "0363f1f8fc7e3f1f8fc7e3f1f8ff2e1aa2c1aa2c1aa2c1aa2c1aa2c1aacc5b03a822eb40"
    "03e3f1f8fc7e3f1f8fc7e3f1f8c186a8b06a8b06a8b06a8b06a8b06ab31540ea1a6b8280"
    "09a80f8640700101f91000363f1f8fc7e3f1f8fc7e3f1f8ff081aa2c1aa2c1aa2c1aa2c1"
    "aa2c1aacc2e0760ceb83001f1f8fc7e3f1f8fc7e3f1f8fc6a03545835458354583545835"
    "458355985c0ec19abd6003c3f1f8fc7e3f1f8fc7e3f1f8c6835458354583545835458354"
    "58355987036068d9a01b0342e4000e0fc7e3f1f8fc7e3f1f8fc7e3f43035458354583545"
    "8354583545835598181a833c8100d419d73800d8fc7e3f1f8fc7e3f1f8fc7e3fd586a8b0"
    "6a8b06a8b06a8b06a8b06ab3300d40e1780350385e6003e3f1f8fc7e3f1f8fc7e3f1f8c9"
    "86a8b06a8b06a8b06a8b06a8b06ab30e06a048bf81a8122f3001e1f8fc7e3f1f8fc7e3f1"
    "f8fc60c354583545835458354583545835599a81a81448d00d40a179800e0fc7e3f1f8fc"
    "7e3f1f8fc7e3f45c3545835458354583545835458355988036390a01b1ae7001b1f8fc7e"
    "3f1f8fc7e3f1f8fc7fd41aa2c1aa2c1aa2c1aa2c1aa2c1aacc1006c0cdd9501b03357d00"
    "07c7e3f1f8fc7e3f1f8fc7e3f1980d5160d5160d5160d5160d5160d5662703b065ae9c00"
    "787e3f1f8fc7e3f1f8fc7e3f1a80d5160d5160d5160d5160d5160d5660101d830ba8001c"
    "1f8fc7e3f1f8fc7e3f1f8fc7e8206a8b06a8b06a8b06a8b06a8b06ab300c0ec971a001b1"
    "f8fc7e3f1f8fc7e3f1f8fc7fa70d5160d5160d5160d5160d5160d5661601d41957280011"
    "c07c3202800f0fc7e3f1f8fc7e3f1f8fc7e3261aa2c1aa2c1aa2c1aa2c1aa2c1aacd3503"
    "a9ae6400383f1f8fc7e3f1f8fc7e3f1f8fd2b0d5160d5160d5160d5160d5160d566f81d0"
    "36db9c006c7e3f1f8fc7e3f1f8fc7e3f1fe2035458354583545835458354583559a4c0e8"
    "186ea0";
static char const ccg31dfill[] =  // 806 bytes
    "0001f1f8fc7e3f1f8fc7e3f1f8fc6341aa2c1aa2c1aa2c1aa2c1aa2c1aacd12039813b71"
    "5001e1f8fc7e3f1f8fc7e3f1f8fc69035458354583545835458354583559a0c0e80a1b88"
    "0001c1f8fc7e3f1f8fc7e3f1f8fc7eb50d5160d5160d5160d5160d5160d566860740536f"
    "c001b1f8fc7e3f1f8fc7e3f1f8fc7f970d5160d5160d5160d5160d5160d5662d81d41175"
    "a001f1f8fc7e3f1f8fc7e3f1f8fc60c354583545835458354583545835598aa0750d35c1"
    "40013501f0c80e000101f9100001b1f8fc7e3f1f8fc7e3f1f8fc7f840d5160d5160d5160"
    "d5160d5160d5661703b0675c180001f1f8fc7e3f1f8fc7e3f1f8fc6a0354583545835458"
    "3545835458355985c0ec19abd60001e1f8fc7e3f1f8fc7e3f1f8fc6341aa2c1aa2c1aa2c"
    "1aa2c1aa2c1aacc381b0346cd00d81a1720001c1f8fc7e3f1f8fc7e3f1f8fc7e8606a8b0"
    "6a8b06a8b06a8b06a8b06ab30303506790201a833ae70001b1f8fc7e3f1f8fc7e3f1f8fc"
    "7fab0d5160d5160d5160d5160d5160d566601a81c2f006a070bcc001f1f8fc7e3f1f8fc7"
    "e3f1f8fc64c35458354583545835458354583559870350245fc0d40917980001e1f8fc7e"
    "3f1f8fc7e3f1f8fc60c354583545835458354583545835599a81a81448d00d40a1798001"
    "c1f8fc7e3f1f8fc7e3f1f8fc7e8b86a8b06a8b06a8b06a8b06a8b06ab31006c721403635"
    "ce0001b1f8fc7e3f1f8fc7e3f1f8fc7fd41aa2c1aa2c1aa2c1aa2c1aa2c1aacc1006c0cd"
    "d9501b03357d0001f1f8fc7e3f1f8fc7e3f1f8fc66035458354583545835458354583559"
    "89c0ec196ba70001e1f8fc7e3f1f8fc7e3f1f8fc6a035458354583545835458354583559"
    "8040760c2ea00001c1f8fc7e3f1f8fc7e3f1f8fc7e8206a8b06a8b06a8b06a8b06a8b06a"
    "b300c0ec971a0001b1f8fc7e3f1f8fc7e3f1f8fc7fa70d5160d5160d5160d5160d5160d5"
    "661601d419572800011c07c320280001e1f8fc7e3f1f8fc7e3f1f8fc64c3545835458354"
    "5835458354583559a6a07535cc8001c1f8fc7e3f1f8fc7e3f1f8fc7e9586a8b06a8b06a8"
    "b06a8b06a8b06ab37c0e81b6dce001b1f8fc7e3f1f8fc7e3f1f8fc7f880d5160d5160d51"
    "60d5160d5160d5669303a061ba80";
static char const ccg32d[] =  // 774 bytes
    "001f8fc7e3f1f8fc7e3f1f8fc7e31a0d5160d5160d5160d5160d5160d5668901cc09db8a"
    "800924924924924924922903548b06a9160d522c1aa4583548b06a9668303a028800f07e"
    "3f1f8fc7e3f1f8fc7e3f1fad435458354583545835458354583559a181d014dbf0009249"
    "2492492492492525c3548b06a9160d522c1aa4583548b06a9662d81d412003f1f8fc7e3f"
    "1f8fc7e3f1f8fc60c354583545835458354583545835598aa0750d35c140044d407c3203"
    "800c07e440008d8cf8cf8cf8cf8cf8cf8cf8cf8cf8cf8cfe4206a9160d522c1aa4583548"
    "b06a9160d52cc2e0760cf001f8fc7e3f1f8fc7e3f1f8fc7e3501aa2c1aa2c1aa2c1aa2c1"
    "aa2c1aacc2e0760cd5eb001249249249249249244683548b06a9160d522c1aa4583548b0"
    "6a9661c0d81a0ecd00d81a2003c1f8fc7e3f1f8fc7e3f1f8fc7e8606a8b06a8b06a8b06a"
    "8b06a8b06ab30303506790201a833ae700124924924924924924a5586a9160d522c1aa45"
    "83548b06a9160d50881a81c2f01c007e3f1f8fc7e3f1f8fc7e3f1f8c986a8b06a8b06a8b"
    "06a8b06a8b06ab30e06a048bf81a8122f3001249249249249249244186a9160d522c1aa4"
    "583548b06a9160d5903502891a6003c1f8fc7e3f1f8fc7e3f1f8fc7e8b86a8b06a8b06a8"
    "b06a8b06a8b06ab31006c721403635ce002492492492492492494d41aa4583548b06a916"
    "0d522c1aa458354b30401b0334eca80d819b001f8fc7e3f1f8fc7e3f1f8fc7e3301aa2c1"
    "aa2c1aa2c1aa2c1aa2c1aacc4e0760cb5d3800924924924924924922a03548b06a9160d5"
    "22c1aa4583548b06a9660101d831001e0fc7e3f1f8fc7e3f1f8fc7e3f410354583545835"
    "458354583545835598060764b8d00092492492492492492529c3548b06a9160d522c1aa4"
    "583548b06a9661601d41960031c07c32028008f0cf8cf8cf8cf8cf8cf8cf8cf8cf8cf8c9"
    "30d522c1aa4583548b06a9160d522c1aa59a6a0753800f07e3f1f8fc7e3f1f8fc7e3f1fa"
    "561aa2c1aa2c1aa2c1aa2c1aa2c1aacdf03a06db738009249249249249249252203548b0"
    "6a9160d522c1aa4583548b06a9669303a062";
static char const ccg32dfill[] =  // 785 bytes
    "0001f8fc7e3f1f8fc7e3f1f8fc7e31a0d5160d5160d5160d5160d5160d5668901cc09db8"
    "a80001249249249249249245206a9160d522c1aa4583548b06a9160d52cd060740510001"
    "e0fc7e3f1f8fc7e3f1f8fc7e3f5a86a8b06a8b06a8b06a8b06a8b06ab34303a029b7e001"
    "24924924924924924a4b86a9160d522c1aa4583548b06a9160d52cc5b03a824001f8fc7e"
    "3f1f8fc7e3f1f8fc7e3061aa2c1aa2c1aa2c1aa2c1aa2c1aacc5503a869ae0a00113501f"
    "0c80e00180fc8800011b19f19f19f19f19f19f19f19f19f19f19fc840d522c1aa4583548"
    "b06a9160d522c1aa5985c0ec19e001f8fc7e3f1f8fc7e3f1f8fc7e3501aa2c1aa2c1aa2c"
    "1aa2c1aa2c1aacc2e0760cd5eb0001249249249249249244683548b06a9160d522c1aa45"
    "83548b06a9661c0d81a0ecd00d81a20001e0fc7e3f1f8fc7e3f1f8fc7e3f430354583545"
    "835458354583545835598181a833c8100d419d73800124924924924924924a5586a9160d"
    "522c1aa4583548b06a9160d50881a81c2f01c001f8fc7e3f1f8fc7e3f1f8fc7e3261aa2c"
    "1aa2c1aa2c1aa2c1aa2c1aacc381a8122fe06a048bcc0001249249249249249244186a91"
    "60d522c1aa4583548b06a9160d5903502891a60001e0fc7e3f1f8fc7e3f1f8fc7e3f45c3"
    "545835458354583545835458355988036390a01b1ae7000124924924924924924a6a0d52"
    "2c1aa4583548b06a9160d522c1aa598200d819a765406c0cd80001f8fc7e3f1f8fc7e3f1"
    "f8fc7e3301aa2c1aa2c1aa2c1aa2c1aa2c1aacc4e0760cb5d38001249249249249249245"
    "406a9160d522c1aa4583548b06a9160d52cc0203b0620001e0fc7e3f1f8fc7e3f1f8fc7e"
    "3f410354583545835458354583545835598060764b8d000124924924924924924a5386a9"
    "160d522c1aa4583548b06a9160d52cc2c03a832c00018e03e1901400011e19f19f19f19f"
    "19f19f19f19f19f19f19261aa4583548b06a9160d522c1aa458354b34d40ea7001e0fc7e"
    "3f1f8fc7e3f1f8fc7e3f4ac35458354583545835458354583559be0740db6e7001249249"
    "24924924924a4406a9160d522c1aa4583548b06a9160d52cd260740c40";
static char const ccscan[] =  // 7890 bytes
    "26a0640d4fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffff90330d5e24390c806ae21a9843"
    "20266103508dec285d1818d24c387d20e964338f903c0ac7c8640138fa4ffffff41a5d75"
    "f7c7ffd7fffefd75d76df5d75e1892f27e4fcd64e84bc94c950979051304500412059784"
    "126414717c41e645f358448165e107992f3592e2f886650859904a104f29c22401301be4"
    "80ff408a8f08dcfc236c3d708be1846e7a2f8611b61e81150b845f0c2373d022a345f0c2"
    "38d846d868c30c230635d830e0e0e82b061060920d8308304906c19b02e6c1e826d20d87"
    "4136906c1d0560cd83d04da41b0741587413682b4836a1b483833604c0ccb7f1add24ddd"
    "24dfdba4de93a4dc6bdba4dc6ba4e92a4ddb75fb7f4eea95baa57fe95c7a574ffa574f1f"
    "ba575ff86ff1bfefffafebfc7aff1ad5af7ec7c8198369ff74bdd2efffffbffffe87b1e7"
    "c3f20660abdf57abdebffabfaf5fabd7ebf2817bffa51ba5fff1ffffc7ffe44437e40cc1"
    "a5dff6f9c0d8ffff9c0d2ffff38047ff0479c37fbffa5e97ffffffffffa0b06ffff7ddbf"
    "fc9bfbfff937f7fff416417bc9bf7ff75c53ffffeffffff6ff921dffffd8ec8477ebffb2"
    "11ffaffec847ff211fa5effed275e9fa6feff7fdbfbfdffdd26fffbf86baeeda4eda5bd7"
    "b697b696f5eda5fb7b693a23add3d7045624da2e818228062c30953612040ed4206081b0"
    "c24103040db0902280ad4206081b0c2408a0406081b0c24d84986129fc35b5e831098af6"
    "83109c310b0c57862178621618af0c42fc310831098a791f1083158888888e2222222222"
    "22222222222223a59035077f4d2ffd2da096c42c7fffffffffffffffffff20660d29cd64"
    "36ac86659051640f06f640f0cee43200c06432017c20d3641b6c813d903c17b20db64332"
    "c86de730cdb870e4e03870f640f03780660830e1c3870e1c940745f7a09a0ffe4303ffe1"
    "8641b477fc82a0e43287ff2181ffaff7ff0effffffbfa43fb23be3ff8edffffe3fffffff"
    "ffffffffffafa5ebfeabfaf8e39021f205dff204410cb8215f20bb90ae41a1f23821b6e4"
    "35dc835c7927c82390647c8378c830f9021c858c82eff9021f205dff2163f3f0411e32ec"
    "8b329f3f0468e7e08119c9d978102340411e32f8208f0104782ecaf0823c1acf690408d0"
    "408b3f0208ee4acfc7e0823c1022c20468d20823c17c571f0234208f06b3d8208ee4acfc"
    "45fc20468cfc10478cfc0823b8408d197c104782ecaf08d0083439f8208f193b23994f9f"
    "8234020ccc119c9d9eff447748378745daad11dc32e2511dd20c8e507483a41bd8248369"
    "06c3649d20d86195029070e88ee826c3a23ba239b48361d20fd241b0c20c20d065c220d8"
    "61950826c3a23ba2e5483e88ee906f44774136907d8248361b24e1970305c2511dd20de1"
    "d176ab44770cb8182e2a0c8e41951f6da5fa58db7db695fd697d25a5d5a5d3d76ddb76de"
    "97d782497f5a5d3db76da5d76da5db76dd7d25d5fdb697e9636dfa574ffebff7ffab7f7a"
    "fa4bafbaf8dffffafdf3317ddfaf8ffef7fafff7e92fbffaffdffab78fd74dffd7d75fc3"
    "1a6f4a9ba6fba6ea18d63d7d37c3198044dbe974dd0f5af0c6ba6eb1e18e9537dfd74dff"
    "d7f5d7fc7fd7fffecfb1e1638fe3d9f0d9c89dfc7b3e0df1e8114ff1e435bbfd9f2f1f91"
    "399f0d90b1fffc7fd7fffff2e19bdbfffff0dcb866b3bcb852e0fe5c121bf08bbff2e199"
    "0df2e187b6972e0908bbff86fe5c337845dc376779707fff2e199dbfffffffed2fcb1fff"
    "c858ebfff20a23c25ffe43c7bff963f84bff21951ff84b2162bff2c7cb1fffb4bf2c7fff"
    "fbf40820bffffcb1def6eefef20b8eff5ff7931dfbd27fefaffc8671dfefeb2c77dbbfff"
    "fbf40820bfffffdc8a3fe3ffedaffb9147db7088fc8a3f4e451fff5ce3fee451ffdc8a3c"
    "70fdc8a3eb9c7f7ffdc8a3fae105fb6e451fafff7228ff8ffedafffa5f7fffd6b7e977a5"
    "a56ba5dfffffa5dfe93ffa59091ffeb7fe97ff7f7a56bfffa5f7fff5eb7b7fb331bdbef6"
    "d26d0229fa6fdb6f6fd26ffabdd7bf6ffadbe9dbdbfdd7bdb408a7eaf6fbdd7eb6dbf49b"
    "dbef6ff66637b7db69790330d22020c106cf867061038610608383083041b14ce61d15cc"
    "f867068367c8f9229d04cfc0e8ae083041b3d86106083081b3e0403a2b810367c26a0d9f"
    "81ecf61841820d9ec3a2b820c106cf8670c106cf61d15c10341b3e48a74141c1841820d9"
    "f0201840e1841820e0c20d8a081e22222288e23111111111111111c30844444444444444"
    "44444444388888888888888888888e184222222288e23111111d5f1f157ebfebed74bb5d"
    "b4bf6d2c34103482061ac44447ffffc81a06c1c8145902b4e6b2190ec864815e1037c206"
    "3083d9069c20cf6436bf20660d4411f117320d701e619b70e1c81e1b2081e1203870e1d1"
    "7d8708b9f4917323911ccb87f90c0e82683ffbbbfe40dc7fb20d03906b1fd20a93abfd7f"
    "bbffa43e957f238892d6f1ff6477c47fffc7fd2c7d0229fffffffffc74bb4bd7ffffd7d5"
    "74b5fc8123f909038e4087219e0813be433b9061c8dc81e12081e3720c3e433be457f219"
    "df21208177f2043e432e321b232090415e34b3dfa590c59f89d916653e7e08d20468f3f0"
    "411e020468359785d9ecfc5d8411e0be2ef2bc208f04a6538208ee4acfc43164ecfc1023"
    "41acaf2767b3d9767e22cca70411dc959f88bf8411e0bb3f117f0823c11ff95e1047808c"
    "d823392b3d8411e023439f8208f0110e379b811a023407046808d08119c9d9ecf67e3f04"
    "08d1aaa23b2efa58744770e8bb55a23b865c683fa23ba41b48387070c32a0309a4836184"
    "d592748368bb54826c3a23b870e88ee9070d9270c32a0387447745daa41361d11dd172a4"
    "1b0e88ee8b9520da2e5b24e906c32e34191c61950906c32e1288ee906c32e38619509070"
    "cb81083865c220c8e419501d11dd11cda41fab6ffedbd2c6dbebedb4bafe9fa4bea9b4b4"
    "b1b6edbedbaeae9fb6d2c6dbb6d2d2edb4b4b4ba6d2f4ae9d2fb6d2fe9ebebd2ba7b6faf"
    "5afd2ffeffbff5effc5b4976d2f5fbfffefdf1ffbffdd7fdd7dfd7d5bc57fd7fc6fefd5b"
    "c7fbffbf4baffaf863d74dc31fafa6ffba6fc7afae18f75d7e3d6bd375af4dafdd37d754"
    "df5d37f50c7863d755f0c7afffffafd9f0d4fc6cf97ef8db4be3e884effd9f2fffd10d1d"
    "ff1ffc7ff1ffc7fc7fb3e567cbffecf81982b57fd2ff6ffc37fcb8177fba52e0bff97047"
    "845dfff0dfffb7845dffe5c1ffcb87ff2e09fe5c0bfe5c0bfc3786ffff0dfbff107fda5f"
    "9639123fc86647ffdffda412fff21b23ffed2097ffffffffff963fff90c4658e4344658f"
    "ffe42c7fbd10affa04105fe71dffb21a077fdb4add2fb40820abffc838efffe8104157ff"
    "7ff7ff7ff7ff7f91477f2083bffff963bff5fff8ffffee451ffff7228ffd382058ae71ff"
    "ffebff15ce3fbee451fdf70447bf4e451f6d77228ffee451ffffff6d7fff56ffffbffbcb"
    "8ff4afd78f49bb5d2bffffbed7fefffd74bf5d2d6d74bebd2ffd2feffbffaffbcb8fd26d"
    "2efd998dedfd6b7b7f4da0453ed37fd37b332af7f7f5a5f7b3305ebdeda0453b7deda045"
    "3b7b68114fd37db696dedf7b7ffd37f4deda5dff5fb693690206106083081c3083041c1d"
    "15d44141010608367c1115cce61840f67c2611f99f30d9ec3083041840c20c107457514e"
    "82081840c20c1070d9ec3083041b3d86cfb0c106cf61b3e33d8708fccfc1b1410367c180"
    "610608367c1c1820c20745741d15d06c5040c20c106103a2bbda0c20c211114471188888"
    "888888888888d088a2388c4444444470c211114471188888888888888888888888888888"
    "88888888888887c4455f57c55feb4abebfb52192dfb5ed52db4b49b4b6d2f0d430d040c3"
    "496222223fffff90330d2c20bd97640f054b206f640f17081e18b21920f640e2c811e561"
    "03c3e103c19f0818cd17d870e436a038745f61c38727011736780e433c070fffe40e07ff"
    "20798f903cc720786c1c8368fffbbe451ddf487dff487fff749c77fffc7fffff1fc7ffff"
    "fffffffffd7f5d557ff5ff2163c84820786efe4087205e320c4102ef906f1e41872196e4"
    "48c8343e4123c830e432e3f22c10d37f21aee42c10b1911f205dc8653e11998233f8408c"
    "e5f15c7c3d9767e22cca708d0e7e0823c0408d19f8204672567b0823c04668de6cc119c9"
    "d9f8fc11a1cfc104780823c0408d011a46d9c208ee4acf67e2ec2342046708d0e7e0823c"
    "0408cf97610830811a0104772567e22fe408b08d018379b308f0102338408ce4ecf64ff0"
    "823c12fc208f06e2ec208f05d9ecfc5de1971506472e90647308308341c3a23ba2ed5065"
    "c2511dd20da41f4477483238c32a120d865c61950906472a23ba239b0cb84a23ba41b483"
    "69070cb8483a09b0c32a0309a0cb844191c36170944774836937870cb8c1241d04d87447"
    "745ca1c32e05c32a120da4da4191c832a1172a41b45ca906c309a48361865406135e95fa"
    "57f5db6963db6975db695d3a5f4e95db7f6da5a5d7fdb74fd695f6da5a4bff5db76da5fd"
    "3a5a4a95d3a5a5a5a5f4974fd7ab7eadddfff7faf7fab78afe2adfffaebdfffe2da5ab7f"
    "afbffdfffbfe2beeade3bafbaeda4be2da5f5fd6fa5d7fd74dc31aebaa6faaebfae9ba6e"
    "18fe3d7f5f5d36a3ff0c47ad7faa6d46ba5e9b5e9bfa6ebfffff408a7ffebf8d9f2ffc7f"
    "ffff1c6cf97c89db697ffc7ffb3e0d7fffe3fff8fe3be3db4bfffef6d2fdbff970cd7ffc"
    "b866fffff970cb2e0eff08bbfffff2e197ff0d845dffff970bffe5c2f970c529707fffff"
    "bfffda5963fe41ae3ffffffcb1ffc8111963f84befcb1ff2c7ffff21e025fff963ffffff"
    "ffdfffffa4ffe810417fd90303bfff7fffffdd9083bffd7ba5ffff7ff931debffffbfffb"
    "fb6d2bdd2f6d7db58e1ff8ffb9147ff6d77228fedafffee451dc20bffd738fffb6bfee45"
    "1dc8a3fffae71fdfff7088fc111e6bbee111fee451fdc8a3fffd7f4fff7ffa57ff5e97f5"
    "fffe9695ffaffb7fd7fe9697fde68fff5ffd2d2ebd74b5d28f4b6fb6d2f6d2a7ef6660a1"
    "f7b7f57b696df7b695fb7dededfd37b68114eebfb7b6937dededfffd6ebdeda0453edfb7"
    "b7b696da0453b7b68114eded37fe106c5040c206c53d040c20c1070e0c20c106cf8108ae"
    "083041b1410367c338610362820c10610383083041b3e0dcfc8ae836730d9ec3081f06c5"
    "41841820d9f06e7e0610308182074576cf61841820d9ec3081c1840d9f23e31410367b0d"
    "9f23d86cf8967d83f111111111144711888888888888888888888888888888d088888888"
    "8888871111111111111111111111ad5ffeb4b4bb52066048fa4416c155fdb4b4bd2c350d"
    "2878888fffffe40cc1a2c864865610c802bbc3903c15603903c33c07fc8681c851df2078"
    "660ffbfeffe3f8fffffeabafe433b90ee412084820b0fe4359f20a904120814390623c81"
    "0e4369fcaf0823c0208ee102338408ce4ecfc40c58411e02348d33ccd8384772567e22fe"
    "41ecfc7e08119c204672567b2ac9d9f88b329c204672767e08d0e7e0823c104e2b8f8082"
    "3b92e2efb24e906d04da4da4191ca88ee1d20d865c2171026904d87447745ca1d11dd11c"
    "da4191c683238c32a0387447745daa483239511dc32e1288ee906c30830834826c309ae9"
    "b4bb6d254aedbd2ffab6edb4bb6f4ad2ba7edb4b14aedbedb4bfab6fafd7fdd5bfd7f6d2"
    "ffdff56d5bc7fed5bffd7bbfdb4bee9b1d46bafa6ffc7ad7afaebaebfaebeba6df487ffe"
    "323efff8fdb4886ceffffffffafffe3d0229f2273697f2e182effff2e09fc22effffffff"
    "b7ffff2e05deda41177fff097fffe58f963de12ffffffffb4bff2c7fdff0977ff75fffdf"
    "fba55fffffffe810417fffde93f5ba5f4e451d7088fc204dafee451fffd738feffedab6b"
    "ff8b6bffee451e386b9c7fed74bfd2ebc947a5fedfffaffd75ff7d7ffe93ffb6471f4dee"
    "bb7b695fedededfbaf7b68114f7eda4da5f7b33061a57b7dedf4eebff08fccf91ec367c8"
    "5041820fd9f20707b3d8610608367b0c20c1061036298a081840c20c1070d8a083041c18"
    "41820d9f0cfa67b0fac4444444443e223422222222222222288e2311111111111a1ff57f"
    "c820fa55a5a49e41b1fb521900effd56936969693070c350f11111fffff20680e9cd6432"
    "41670865d9066b2191390c80cdc21ec8640126730cdb870e1c3903c32a2d17d87205101f"
    "41341f72040f90691c8371f206e390ca1fffdfafffffdc3a43bffb23bffff8fe3fffffff"
    "ffffebaaeaa97f8e39021c86c8204ee432a0837820a1c8677219dc857fc830ff9021c86c"
    "8c823e436464704191c82bb90d28f20bb90ee40adc834390f04246470436dc878fcfc104"
    "780811a0d65e1767b3f17610478371761023393b3d8408d00823b9126539567b0823c041"
    "1e08ff08d0e7e0411dc234073473f0411e02046808d0c23b84691b1823393b3f24102341"
    "0c59f8fc0823b92b3f13b3f0411e0d67b08119c2330c4be0823c0411e0d65793b2ecfc45"
    "994e412cf61047808119c204672567b2ff0811a083f15c7c0411dc20468fd11dd20da41c"
    "383861950184d241b0c26920c8e4195090741368bb5418654241b48368b9432e1288ee82"
    "6c32e1b0b89447748369070cb8813619709483239511dc520e1d11dd11cda09b0e88ee1d"
    "11dd20d8619509064722e19ec1241b48361b24e1c3a23ba2ed5061950906d26d20c8e30c"
    "a845ca9070c20c20d209b483fdb6975fd3f497d2574f5db69634e969697db76dfdb6975f"
    "6dfa576dd76df6ddb76da5d3a57f4969757db69634e9692a574e975fd5b75ffaf7fe2da4"
    "bb692b78dffde2bafbffffebdfffab7f7ffffebe2adfa4bafbffde2beeade3bdf77fdff5"
    "d370c7ebe9bfaea188ff54dd36bf58ff5d370c71febae18d78f5d74dd57e95374df7d7f5"
    "4da8d74bc317d21e18ffc6cf97ef8db4bf67c367ae38fff22f7fc6cf948277ffb3e047c8"
    "9dff1ffe1638fffeb8fffd9f0228114f91399f0330d5ff2e194ff74a5c31ff0d845ddbf9"
    "70a5c3ffc22efff2e194f08bbfff86ff08bbffcb827fb3bcb872e09ff6fe5c3fff0ddeda"
    "411770dffe41a23ffeffc862025da5ffe58fe12f2c7cb1ff20d1196384bcb1fff219e3f0"
    "97ffffd7fffda5fffe42c3fe12c878ffb20dc77fdb4add2fc8a3bd681041777fff5fffd9"
    "06e3bfd7fff90e3bff5ffdffedddffe8104177ffe58efa4fd64c77ff7228ffffb9147f6d"
    "7f5c102c77088fc111effeb9c7fff7228ffeb9c7fdb5fffd738ffee451f6d7db70447a45"
    "1faff1dc111e8409b5dff8e1ae0817ffe95faf1e937d77ff7e9696bffffffe95fffffd7d"
    "ffffffa5f5f7a5a56bfdfa5a5d7adefff7eb7b7f4da0453ed37eda5f4ebb331dbdbdb408"
    "a76fbdd76f6fbdbfa6f75dbf6d2bfabf75efbdbf6d2bdb6f6fd2ef66619edededa5b6811"
    "4fd53bafd640d0182020c106cf86115cce61840f67c4b14103a2b99ec38610367c8f8cf6"
    "1c1841820d9ec38383083041b3e1845741b3d870610362820c107457041820c206cf6184"
    "1820c20c106cfc0d8a083081a0d9f19f3453a082061060838610367c67c85040d9ec3a2b"
    "9e99ec3a2bb11111111111a1111114471188888888888888888888888888888888888888"
    "888888e18422288e23111111111111111f57c55f4abebc81b0087ed7b5f49b4b6d2e1868"
    "2061ac4447fffff20680a9640c5904b20784f2a640f05f0866590c80518432015fca9903"
    "89907e3f903c0bf958418f276432b08665e1c38708b9891dc3870e4e082a41057808b989"
    "1dc22e3611731239b08b9b3c045ce1c387ffe9dc8671f20786d0ef90d31ffa74987d2905"
    "91fdfbdc8681fff49fffdf7749f49d2fd2714abfffffffc711fd5bdc7fc7ffffffffffbf"
    "ffffffff5d75fd2fd7ff5f20660aee4355f2187f2056ff908e41adff2043906f042b90b0"
    "4143e40c3e4087216321a8e41a2082abfe447dfc8310423906f19047ff211ff2043e40cc"
    "1a2cf6104782ecfc7e0411dc959ef2f810477cafc9f17641f8ae3e0208ee108c208f0082"
    "3b92b3f117f08d0e7e0823c0408d01047808119cbb3d8411e023406023b909f976104780"
    "8119c234cd99e66d847820bd8408d0417e2b8f80823b84230823c7610938c119c208f010"
    "2340468611dc2106110ca1047825f8468e7e0823c618654241b0e88ee88e6d04d861951b"
    "04826ec93e184d06106106904d865c65ca906d04d87447745ca197094477483690748369"
    "3618654241b0cb86c09b45ca1d20da4d865c486134906c3a41c30830834826c32e32e548"
    "37e1971972a4da41b483865c409b0cb8c1065c7d20da2e50cb8944774837d3a5db7db74f"
    "d5b74dfd7d5b7fa5db76da5f6da5d69692d3a5f6da5e9692ff497d7f56dfe97ffa4a975f"
    "6dffe9697db697e2bfff8e97feda577ffebffeffaf7afbe2bffbebefeda4bf7ddfffafff"
    "eebdffffebeffafd5375e3d690fdfefa43ff4d8f5afd74dc31a6d46a9bc75fa6d47fe9be"
    "18be90ffd37ffa8d370c71fffa6d7eba6fe3fc89de1485efdb4b408a7c89dfc64333bfff"
    "8d9f63f8f2063bf8ffbe3d9f0d1408a7c89dfc7fff1b3e5209dffc7ffc7f2e0ff08bbf67"
    "708bbfff7b69045dff970d42effffcb865b970be5c121177fe5c2fdd2970ca86ef6d208b"
    "bff2e1b54bfe5c32de1177ffe5c2ffe5c0cc157ff84beb097fdeff84bff097ff963fe41a"
    "63ff2c7097ffe58fff90b0ff84bff6fffc834c658e12fffff2c7ffeffafdbaff74b49faf"
    "fbaffffec8281defefebfeffdb4af2c77d27ebfefaffb20a077fafffbfffbf7228ffae71"
    "fdb5ce3f5fc70d738ffb91475ce3fbffee451fdc223f228fb9147f5c111efb8447e451ff"
    "f7228ffc70d738ffb914775fdc223f228ffeb9c7ffdc223fffdc8a3fe97fffdff6bb6fff"
    "fe9171ffebffa57e9697a5ffebe9697f1e977bffffa44e1b4bfd2d2bffffff4b5ffd2f6f"
    "bf75fdb75fafa775ff6fdd7bdb408a76fbdbfa6f6fdbdbdd76d0229f6f6f6fda6ffaa775"
    "ff6fdb4bf6f6fe9bdd7ffdbdb408a76fbdbf081b3ec3041840d9ec3081840d0657020e11"
    "f8207bd33d86103041841b3e767b0c20c106cf61c1841820d9f05d15ccf91f60d9f206cf"
    "61b3d87b3e47c60c207b3e0b83a2b9e99ec3081820c20d9f24c341040c106106cf91f05d"
    "15d06cf61840c103081820c206cf91ec383083041b3e0661ab111111111111111a111111"
    "110e22222222222222222222388888888888888888883084444444444444444444447fc7"
    "4b21920583f90350d37d24ff4baae1a4c1e223ffffe40cc14f8fe413081e0576436bc86a"
    "d9066b20a5840aec82a610238fe434ef08b9891cd8721a70435e03845ce1c3a2fb0e1c9c"
    "045cc48e6c3903c3323e95f231f21a63fdff9c720ac3eff4b9147f4bf7dfd2af487fdd2f"
    "f7f71c71fc7fff1dc7c7fffffffffff55ffd57ff5c819869bfe447c858f2043e4349f20d"
    "8e43be4351fc823f9021ff20c47906f04144109044821b4f8208ee10938c160823b93e2e"
    "cf611a3046820964ecfc11a39f8208f0110d9bcf10477c83f15c7c0823c0411e32f81047"
    "708cd1a10478359ec23439f8208f0417fcbb0849c608ce6b2f308d03008ce5d9ec204670"
    "811a020468359767e22cca7d04d865c65ca8b9504d861341c32e2a0e1c3a23b865c4a23b"
    "a41b0cb8e1d11dd04de1841841a4836906f60904d865c65c220d86195019709447748368"
    "b943865c65ca9361c1f0cb8674d86195090647683a41c387447745daaedbfd2edbebebed"
    "bedb4bfb6edbfe92d2fab6fd2e9fb6d2d2ffd25ff4969d2baebedb4b1fffdfdb4beffff5"
    "ffffbbd75f4bfd7c7f5f7ffdfff7c55bbefffbc7fd71ffe18f5f5d37f58fbe95374de90f"
    "f4dd75d36bffa8ffa8d570c618f5fe40cc34fbff2275fecf82fffe3ff22f7a0453f1c785"
    "20d5df1ffc7fffffffb3e67cbfd61177ffc22eee9786fffe5c0bff08bbf7b694b872e08c"
    "ee1177f9704ff2e1b3ffffffe1b0dfedf84bffc25ff9639091fe58ffff84bdfffac25f96"
    "3fe58fffffffe58ffe4482247f6975fff5b697e61dffffbffafd27eef6ebfefffbffffff"
    "ff38ee71dff40820b5ce3fefae71fffffffdc8a3ffae71f8e1b8223d228fb6b9c7fdc8a3"
    "ffee451dfff7228fffb9147db5ffff1ffff5fe3fefffff4bfffdfd2d2ec947ffe97ffa5a"
    "ffe96bfe97d5f7ff641e375ff6d0229dd76b6febbdbef6ffbdd7e9dbdbf6f75edbdbf6fb"
    "dbdb408a7ff6f6d0229f6f6fdb4baebbd998fb3d86103041841b3d86cf61e10383a2b820"
    "610608383083041b3e0e0c10610608367b0c206f4cf8cfc0d07b3d8610706cfc0e0c20c1"
    "06cf82e7b0c206103041841b3e339861038367d8362a8aea2b8206106083875888888888"
    "8888888888888888888888888871111111111111111111111111111111447118fabfd7a5"
    "6beda5a41ac47fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffc813430f85c22ff1ffff"
    "ffffffffffff204d0a7c7fffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "fffff8008008";
// A CMYK JPEG as Ghostscript writes one, Adobe APP14 (transform 0) and
// inverted samples, 16 by 8, cyan then black, its ICC profile (APP2)
// cut out:
//   1 0 0 0 setcmykcolor 0 0 8 8 rectfill
//   0 0 0 1 setcmykcolor 8 0 8 8 rectfill showpage
//   $ gs -sDEVICE=jpegcmyk -r72 -g16x8 -dJPEGQ=90 -o tiny.jpg t.ps
static char const cmykjpeg[] =  // 360 bytes
    "ffd8ffee000e41646f626500640000000000ffdb00430003020203020203030303040303"
    "04050805050404050a070706080c0a0c0c0b0a0b0b0d0e12100d0e110e0b0b1016101113"
    "141515150c0f171816141812141514ffc000140800080010044311004d11005911004b11"
    "00ffc4001f0000010501010101010100000000000000000102030405060708090a0bffc4"
    "00b5100002010303020403050504040000017d0102030004110512213141061351610722"
    "7114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a343536"
    "3738393a434445464748494a535455565758595a636465666768696a737475767778797a"
    "838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2"
    "c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9"
    "faffda000e0443004d0059004b00003f00fcaaafd53afd53afd53afeaa28a2bf957affd9";

static Str pihex(Arena *a, char const *s)
{
    iz n = 0;
    for (; s[n]; n++) {}
    Str r = {allocbytes(a, n/2), n/2};
    for (iz i = 0; i < r.len; i++) {
        u8 hi = (u8)s[2*i];
        u8 lo = (u8)s[2*i+1];
        hi = hi<='9' ? hi-'0' : hi-'a'+10;
        lo = lo<='9' ? lo-'0' : lo-'a'+10;
        r.data[i] = (u8)(hi<<4 | lo);
    }
    return r;
}

enum { CCCOLS = 2700, CCROWS = 24 };

// Stripes whose edges move between rows (V modes), a disc with a hole
// (makeup codes), a black row and a white one (extended makeup codes
// past 2560), and a row of one white pixel then black.
static b32 ccpattern(i32 x, i32 y)
{
    if (y == 5) return 1;
    if (y == 6) return 0;
    if (y == 20) return x >= 1;
    if (x < 160) return (x + y%4)/7 % 2;
    if (x < 600) return (x + y*5%11)/37 % 2;
    if (x < 2650) {
        i32 dx = x - 1900;
        i32 dy = y*27 - 320;
        i32 d  = dx*dx + dy*dy;
        return d<600*600 && d>=100*100;
    }
    return 0;
}

static Str ccwant(Arena *a, b32 blackis1)
{
    iz  stride = (CCCOLS + 7)/8;
    Str r      = {alloc<u8>(a, stride*CCROWS), stride*CCROWS};
    for (i32 y = 0; y < CCROWS; y++) {
        for (i32 x = 0; x < (i32)stride*8; x++) {  // the padding white
            if ((x<CCCOLS && ccpattern(x, y)) == blackis1) {
                r.data[y*stride + x/8] |= (u8)(0x80 >> (x%8));
            }
        }
    }
    return r;
}

// Rows, good rows and the error of a decode, and whether its bits are
// want, or "fail: why".
static Str ccshow(Arena *a, Str data, CcittParams p, Str want)
{
    i64   work = I64MAX;
    byte *beg  = a->beg;
    Ccitt c    = pdfccitt(a, data, p, (i64)1<<26, &work);
    if (!c.bits.data) {
        Str moved = a->beg==beg ? Str("") : Str(" (arena moved)");
        return concat(a, concat(a, "fail: ", c.err), moved);
    }
    Buf b(a, 64);
    print(&b, (i64)c.rows);
    putbyte(&b, ' ');
    print(&b, (i64)c.good);
    print(&b, c.bits==want ? Str(" same") : Str(" differs"));
    if (c.err.data) {
        putbyte(&b, ' ');
        print(&b, c.err);
    }
    return finish(&b);
}

// Every prefix of fax data decodes without fault, its good rows the same
// as the whole's, its rows all there; and a cut on a byte boundary that
// ends one of the whole's good rows keeps that row.
static b32 cccutanywhere(Arena *a, Str src, CcittParams p)
{
    i64   work  = I64MAX;
    Ccitt whole = pdfccitt(a, src, p, (i64)1<<26, &work);
    iz    full  = whole.bits.len;
    b32   ok    = whole.bits.data && whole.good==whole.rows;
    for (iz n = 0; ok && n < src.len; n++) {
        Arena s   = *a;
        u8   *cpy = allocbytes(&s, n);  // so that nothing reads past it
        copybytes(cpy, src.data, n);
        byte *beg = s.beg;
        Ccitt c   = pdfccitt(&s, {cpy, n}, p, (i64)1<<26, &work);
        if (!c.bits.data) {
            ok = s.beg==beg && !p.rows;  // only a count can find no rows
            continue;
        }
        iz stride = (c.cols + 7)/8;
        iz good   = c.good<whole.good ? c.good : whole.good;
        ok = c.cols==whole.cols && c.good<=c.rows && good*stride<=full &&
             !__builtin_memcmp(c.bits.data, whole.bits.data, (uz)(good*stride)) &&
             (!p.rows || c.rows==p.rows);
    }
    return ok;
}

// Each byte flipped in turn never faults or leaves the bits short, and
// how many decodes report an error.
static i32 cccorruptions(Arena *a, Str src, CcittParams p)
{
    i32 fails = 0;
    for (iz i = 0; i < src.len; i++) {
        Arena s   = *a;
        u8   *cpy = allocbytes(&s, src.len);
        copybytes(cpy, src.data, src.len);
        cpy[i] ^= (u8)(1 + i%255);
        i64   work = I64MAX;
        byte *beg  = s.beg;
        Ccitt c    = pdfccitt(&s, {cpy, src.len}, p, (i64)1<<26, &work);
        if (!c.bits.data) {
            fails += s.beg==beg ? 1 : 1000000;
        } else {
            fails += c.err.len > 0;
            fails += c.bits.len==(iz)c.rows*((c.cols+7)/8) ? 0 : 1000000;
        }
    }
    return fails;
}

// Pixels as hex RGB, the alpha checked opaque, or the error.
static Str pishow(Arena *a, PdfSamples *s, i32 tw, i32 th)
{
    i64   work = I64MAX;
    byte *beg  = a->beg;
    Image p    = pdfpixels(a, s, tw, th, &work);
    if (!p.rgba) {
        Str moved = a->beg==beg ? Str("") : Str(" (arena moved)");
        return concat(a, concat(a, "fail: ", p.err), moved);
    }
    Buf b(a, 64);
    for (iz i = 0; i < (iz)p.width*p.height; i++) {
        if (i) putbyte(&b, ' ');
        printhex(&b, p.rgba[4*i+0], 2);
        printhex(&b, p.rgba[4*i+1], 2);
        printhex(&b, p.rgba[4*i+2], 2);
        if (p.rgba[4*i+3] != 255) {
            print(&b, "(translucent)");
        }
    }
    return finish(&b);
}

static PdfSamples pisamples(char const *data, iz len, i32 w, i32 h, i32 bpc, i32 cs)
{
    PdfSamples s = {};
    s.data = {(u8 *)data, len};
    s.w    = w;
    s.h    = h;
    s.bpc  = bpc;
    s.cs   = cs;
    return s;
}
#define PIS(d, w, h, bpc, cs) pisamples(d, countof(d)-1, w, h, bpc, cs)

static void test_pdfimage(Test *t)
{
    Arena *a = &t->perm;

    // Each code decodes as itself from the tables, so none overlaps: a
    // makeup code followed by a zero-length run, a 1 bit after each
    {
        Arena s  = *a;
        i32  *wt = cctable(&s, ccwhite, countof(ccwhite));
        i32  *bt = cctable(&s, ccblack, countof(ccblack));
        i32   bad = 0;
        for (i32 color = 0; color < 2; color++) {
            char const *const *codes = color ? ccblack : ccwhite;
            iz n = color ? countof(ccblack) : countof(ccwhite);
            for (iz i = 0; i < n+countof(ccext); i++) {
                char const *c   = i<n ? codes[i] : ccext[i-n];
                i32         run = i<64 ? (i32)i : i<n ? (i32)(i-63)*64 : 1792 + (i32)(i-n)*64;
                char const *z   = run<64 ? "" : color ? "0000110111" : "00110101";
                u8          buf[8] = {};
                i32         len = 0;
                for (char const *q = c; *q; q++, len++) {
                    buf[len/8] |= (u8)((*q=='1') << (7 - len%8));
                }
                for (char const *q = z; *q; q++, len++) {
                    buf[len/8] |= (u8)((*q=='1') << (7 - len%8));
                }
                buf[len/8] |= (u8)(0x80 >> (len%8));
                CcBits b = {buf, 64, 0};
                i32    r = ccrun(&b, color ? bt : wt, 2560);
                bad += r!=run || b.pos!=len;
            }
        }
        expect(t, "ccitt codes", istr(a, bad), "0");
    }

    // libtiff's G4, G3 1-D and G3 2-D, with and without fill
    {
        Str want  = ccwant(a, 1);
        Str white = ccwant(a, 0);
        CcittParams p = {};
        p.columns  = CCCOLS;
        p.rows     = CCROWS;
        p.blackis1 = 1;

        p.k = -1;
        expect(t, "ccitt g4", ccshow(a, pihex(a, ccg4), p, want), "24 24 same");
        p.blackis1 = 0;
        expect(t, "ccitt g4 black is 0", ccshow(a, pihex(a, ccg4), p, white), "24 24 same");
        p.blackis1 = 1;
        p.rows = 0;
        expect(t, "ccitt g4 counted", ccshow(a, pihex(a, ccg4), p, want), "24 24 same");
        p.rows = 10;
        expect(t, "ccitt g4 fewer rows", ccshow(a, pihex(a, ccg4), p, takehead(want, 10*338)),
               "10 10 same");
        p.rows = CCROWS;
        p.columns = 0;
        expect(t, "ccitt g4 default columns", ccshow(a, pihex(a, ccg4), p, want),
               "24 2 differs bad CCITT data");
        p.columns = CCCOLS;

        p.k = 0;
        expect(t, "ccitt g3 1-d", ccshow(a, pihex(a, ccg31d), p, want), "24 24 same");
        p.align = 1;
        expect(t, "ccitt g3 1-d aligned", ccshow(a, pihex(a, ccg31dfill), p, want), "24 24 same");
        p.align = 0;
        expect(t, "ccitt g3 1-d fill unannounced", ccshow(a, pihex(a, ccg31dfill), p, want),
               "24 24 same");
        p.rows = 0;
        expect(t, "ccitt g3 1-d counted", ccshow(a, pihex(a, ccg31d), p, want), "24 24 same");
        p.rows = CCROWS;

        p.k = 1;
        expect(t, "ccitt g3 2-d", ccshow(a, pihex(a, ccg32d), p, want), "24 24 same");
        p.align = 1;
        expect(t, "ccitt g3 2-d aligned", ccshow(a, pihex(a, ccg32dfill), p, want), "24 24 same");
        p.k = 4;  // K says only how often 1-D lines may come
        expect(t, "ccitt g3 2-d k=4", ccshow(a, pihex(a, ccg32dfill), p, want), "24 24 same");
        p.align = 0;

        // Wrong kinds and bad parameters
        p.k = -1;
        expect(t, "ccitt g3 as g4", ccshow(a, pihex(a, ccg31d), p, want), "24 0 differs");
        p.k = 0;
        expect(t, "ccitt g4 as g3", ccshow(a, pihex(a, ccg4), p, want), "24 9 differs");
        p.columns = PDFIMG_EDGE + 1;
        expect(t, "ccitt too wide", ccshow(a, pihex(a, ccg4), p, want), "fail: bad CCITT parameters");
        p.columns = -5;
        expect(t, "ccitt negative columns", ccshow(a, pihex(a, ccg4), p, want),
               "fail: bad CCITT parameters");
        p.columns = CCCOLS;
        p.rows    = 0;
        expect(t, "ccitt nothing counted", ccshow(a, {}, p, want), "fail: no CCITT rows");
        p.rows = CCROWS;
        Str blank = {alloc<u8>(a, want.len), want.len};
        expect(t, "ccitt nothing", ccshow(a, {}, p, blank), "24 0 same");
        {
            Arena s    = *a;
            byte *beg  = s.beg;
            i64   work = I64MAX;
            Ccitt c    = pdfccitt(&s, pihex(&s, ccg4), p, CCCOLS*CCROWS - 1, &work);
            expect(t, "ccitt pixel limit", c.err, "too many pixels");
            Arena tiny = carve(&s, 4096);
            beg  = tiny.beg;
            c    = pdfccitt(&tiny, pihex(&s, ccg4), p, (i64)1<<26, &work);
            expect(t, "ccitt out of memory", concat(&s, c.err, tiny.beg==beg ? Str("") : Str("!")),
                   "out of memory to decode the image");
            work = 5;
            c    = pdfccitt(&s, pihex(&s, ccg4), p, (i64)1<<26, &work);
            expect(t, "ccitt work", c.err, "too much work to decode the image");
        }
    }

    // The scan-ccitt case: 1700 by 2200, G4, Decode [1 0]. Its bits hash
    // the same as pdfimages' PBM of it; pdftoppm -mono -r 200 of the
    // page differs from that in 0.4% of pixels, on edges, from where it
    // places the image.
    {
        Arena       s = *a;
        CcittParams p = {};
        p.k       = -1;
        p.columns = 1700;
        p.rows    = 2200;
        i64   work = I64MAX;
        Ccitt c    = pdfccitt(&s, pihex(&s, ccscan), p, (i64)1<<26, &work);
        Buf   b(&s, 64);
        printhex(&b, hash(c.bits), 16);
        expect(t, "ccitt scan", finish(&b), "ec68b848c3b37f47");
        expect(t, "ccitt scan rows", istr(&s, c.good), "2200");

        // At 1:1 through Decode [1 0], a 1 is black
        PdfSamples m = {};
        m.data       = c.bits;
        m.w          = c.cols;
        m.h          = c.rows;
        m.bpc        = 1;
        m.ndecode    = 2;
        m.decode[0]  = 1;
        Image  px    = pdfpixels(&s, &m, m.w, m.h, &work);
        iz     wrong = 0;
        iz     ink   = 0;
        for (i32 y = 0; y < m.h; y++) {
            for (i32 x = 0; x < m.w; x++) {
                b32 one = c.bits[y*213 + x/8]>>(7 - x%8) & 1;
                u8  g   = px.rgba[((iz)y*m.w + x)*4];
                wrong += one ? g!=0 : g!=255;
                ink   += one;
            }
        }
        expect(t, "ccitt scan decoded", istr(&s, wrong), "0");
        expect(t, "ccitt scan ink", istr(&s, ink), "27857");  // pdfimages' too

        // Shown small, gray where text was, and white margins
        PdfShown sh = pdfshowsamples(&s, carve(&s, 64<<20), &m, 1000, 200000, 5<<20, &work);
        expect(t, "ccitt scan shown", concat(&s, sh.mime, concat(&s, " ", istr(&s, sh.w))),
               "image/png 383");
        expect(t, "ccitt scan shown height", istr(&s, sh.h), "496");
    }

    // Cut anywhere and corrupted anywhere: never a fault
    {
        CcittParams p = {};
        p.columns = CCCOLS;
        p.rows    = CCROWS;
        p.k       = -1;
        expect(t, "ccitt g4 cut", istr(a, cccutanywhere(a, pihex(a, ccg4), p)), "1");
        p.k = 0;
        expect(t, "ccitt g3 1-d cut", istr(a, cccutanywhere(a, pihex(a, ccg31d), p)), "1");
        p.k     = 1;
        p.align = 1;
        expect(t, "ccitt g3 2-d cut", istr(a, cccutanywhere(a, pihex(a, ccg32dfill), p)), "1");
        p.rows = 0;
        expect(t, "ccitt g3 2-d counted cut", istr(a, cccutanywhere(a, pihex(a, ccg32dfill), p)), "1");
        p.rows  = CCROWS;
        p.align = 0;

        p.k = -1;
        i32 g4 = cccorruptions(a, pihex(a, ccg4), p);
        expect(t, "ccitt g4 corrupted", istr(a, g4>0 && g4<=763), "1");
        p.k = 0;
        i32 g3 = cccorruptions(a, pihex(a, ccg31d), p);
        expect(t, "ccitt g3 corrupted", istr(a, g3>0 && g3<=795), "1");
        p.k    = 1;
        p.rows = 0;
        i32 g32 = cccorruptions(a, pihex(a, ccg32d), p);
        expect(t, "ccitt g3 2-d corrupted", istr(a, g32>0 && g32<=774), "1");
    }

    // Samples: each space and depth, 1:1
    {
        PdfSamples s = PIS("\x00\x80\xff\x40", 2, 2, 8, PDFCS_GRAY);
        expect(t, "pdfimg gray8", pishow(a, &s, 2, 2), "000000 808080 ffffff 404040");
        s = PIS("\xff\x00\x00\x10\x20\x30", 2, 1, 8, PDFCS_RGB);
        expect(t, "pdfimg rgb8", pishow(a, &s, 2, 1), "ff0000 102030");
        s = PIS("\x00\x00\x00\x00\xff\x00\x00\x00\x00\x00\x00\xff\x80\x00\x00\x80", 4, 1, 8, PDFCS_CMYK);
        expect(t, "pdfimg cmyk", pishow(a, &s, 4, 1), "ffffff 00ffff 000000 3f7f7f");
        s = PIS("\x1b", 4, 1, 2, PDFCS_INDEXED);  // 0 1 2 3
        s.base    = PDFCS_RGB;
        s.hival   = 2;  // 3 is past it: clamped
        s.palette = {(u8 *)"\x10\x20\x30\x40\x50\x60\x70\x80\x90", 9};
        expect(t, "pdfimg indexed 2bpc", pishow(a, &s, 4, 1), "102030 405060 708090 708090");
        s.palette = {(u8 *)"\x10\x20\x30\x40", 4};
        expect(t, "pdfimg indexed short palette", pishow(a, &s, 4, 1), "102030 400000 000000 000000");
        s = PIS("\xa0", 4, 1, 1, PDFCS_GRAY);  // 1 0 1 0
        expect(t, "pdfimg 1bpc", pishow(a, &s, 4, 1), "ffffff 000000 ffffff 000000");
        s.ndecode   = 2;
        s.decode[0] = 1;
        s.decode[1] = 0;
        expect(t, "pdfimg 1bpc decode", pishow(a, &s, 4, 1), "000000 ffffff 000000 ffffff");
        s = PIS("\x12\x34\xab\xcd", 2, 1, 16, PDFCS_GRAY);
        expect(t, "pdfimg gray16", pishow(a, &s, 2, 1), "121212 ababab");
        s = PIS("\x5f", 2, 1, 4, PDFCS_GRAY);
        expect(t, "pdfimg gray4", pishow(a, &s, 2, 1), "555555 ffffff");
        s = PIS("\x00\x80\xff", 3, 1, 8, PDFCS_GRAY);
        s.ndecode   = 2;
        s.decode[0] = 1;
        s.decode[1] = 0;
        expect(t, "pdfimg gray decode inverted", pishow(a, &s, 3, 1), "ffffff 7f7f7f 000000");
        s.decode[0] = 0.5f;
        s.decode[1] = 1;
        expect(t, "pdfimg gray decode half", pishow(a, &s, 3, 1), "808080 c0c0c0 ffffff");
        s.decode[0] = 0.0f/0.0f;  // NaN
        expect(t, "pdfimg gray decode nan", pishow(a, &s, 3, 1), "000000 000000 000000");

        // Stencil masks paint the fill where the sample is 0
        s = PIS("\x50", 4, 1, 1, PDFCS_GRAY);  // 0 1 0 1
        s.mask = 1;
        s.fill[0] = 0xc0;
        s.fill[1] = 0x10;
        s.fill[2] = 0x20;
        expect(t, "pdfimg stencil", pishow(a, &s, 4, 1), "c01020 ffffff c01020 ffffff");
        s.ndecode   = 2;
        s.decode[0] = 1;
        expect(t, "pdfimg stencil decode", pishow(a, &s, 4, 1), "ffffff c01020 ffffff c01020");
        s.bpc = 8;
        expect(t, "pdfimg stencil 8bpc", pishow(a, &s, 4, 1), "fail: an unsupported kind of image");

        // Lab: white, black, and a red past sRGB (Python's float math
        // gives ff001a)
        s = PIS("\xff\x80\x80\x00\x80\x80\x88\xe0\xc0", 3, 1, 8, PDFCS_LAB);
        s.ndecode = 6;
        f32 lab[] = {0, 100, -128, 127, -128, 127};
        copybytes(s.decode, lab, sizeof(lab));
        expect(t, "pdfimg lab", pishow(a, &s, 3, 1), "ffffff 000000 ff0019");

        // Tints: ink is dark, the most of any one
        s = PIS("\x00\xff\x40", 3, 1, 8, PDFCS_TINT);
        s.ncomp = 1;
        expect(t, "pdfimg separation", pishow(a, &s, 3, 1), "ffffff 000000 bfbfbf");
        s = PIS("\x00\x40\x80\x20", 2, 1, 8, PDFCS_TINT);
        s.ncomp = 2;
        expect(t, "pdfimg devicen", pishow(a, &s, 2, 1), "bfbfbf 7f7f7f");
        s.ncomp = 33;
        expect(t, "pdfimg devicen too many", pishow(a, &s, 2, 1), "fail: an unsupported kind of image");

        // A colour key leaves samples in its ranges unpainted
        s = PIS("\x05\x0f\x14\x15", 4, 1, 8, PDFCS_GRAY);
        s.nkey   = 2;
        s.key[0] = 10;
        s.key[1] = 20;
        expect(t, "pdfimg color key", pishow(a, &s, 4, 1), "050505 ffffff ffffff 151515");

        // Short data is white, from the first pixel it does not reach
        s = PIS("\x10\x20\x30", 2, 2, 8, PDFCS_GRAY);
        expect(t, "pdfimg short", pishow(a, &s, 2, 2), "101010 202020 303030 ffffff");
        s = PIS("\x10\x20", 2, 1, 8, PDFCS_RGB);
        expect(t, "pdfimg short rgb", pishow(a, &s, 2, 1), "ffffff ffffff");
        s.data = {};
        expect(t, "pdfimg empty", pishow(a, &s, 2, 1), "ffffff ffffff");

        // Bad sizes and kinds
        s = PIS("\x10", 1, 1, 8, PDFCS_GRAY);
        expect(t, "pdfimg upscale", pishow(a, &s, 2, 1), "fail: a bad size to show it at");
        s.w = 0;
        expect(t, "pdfimg zero", pishow(a, &s, 1, 1), "fail: a bad size");
        s.w   = 1;
        s.bpc = 3;
        expect(t, "pdfimg 3bpc", pishow(a, &s, 1, 1), "fail: an unsupported kind of image");
        s.bpc = 16;
        s.cs  = PDFCS_INDEXED;
        expect(t, "pdfimg indexed 16bpc", pishow(a, &s, 1, 1), "fail: an unsupported kind of image");
    }

    // Soft masks: composited over white, resampled to the image
    {
        PdfSamples s = PIS("\x00\x00\xff\x00\x00\xff\x00\x00\xff\x00\x00\xff", 2, 2, 8, PDFCS_RGB);
        PdfSamples m = PIS("\x80", 1, 1, 8, PDFCS_GRAY);
        s.smask = &m;
        expect(t, "pdfimg smask", pishow(a, &s, 2, 2), "7f7fff 7f7fff 7f7fff 7f7fff");
        m = PIS("\x00\xff", 1, 2, 8, PDFCS_GRAY);
        expect(t, "pdfimg smask rows", pishow(a, &s, 2, 2), "ffffff ffffff 0000ff 0000ff");
        m = PIS("\x40", 4, 1, 1, PDFCS_GRAY);  // 0 1 0 0: wider than the image
        expect(t, "pdfimg smask wide", pishow(a, &s, 2, 2), "ffffff ffffff ffffff ffffff");
        m = PIS("\xff", 1, 2, 8, PDFCS_GRAY);  // short: the image shows
        expect(t, "pdfimg smask short", pishow(a, &s, 2, 2), "0000ff 0000ff 0000ff 0000ff");
        m = PIS("\x00", 1, 1, 8, PDFCS_RGB);  // unusable: ignored
        expect(t, "pdfimg smask rgb", pishow(a, &s, 2, 2), "0000ff 0000ff 0000ff 0000ff");
    }

    // Box averages, straight to the target size
    {
        PdfSamples s = PIS("\x00\x10\x20\x30\x40\x50\x60\x70", 4, 2, 8, PDFCS_GRAY);
        expect(t, "pdfimg box", pishow(a, &s, 2, 1), "282828 484848");
        s = PIS("\x00\x00\x00\x00\x00" "\xff\xff\xff\x00\x00" "\xff\xff\xff\x00\x00", 5, 3, 8, PDFCS_GRAY);
        // boxes: x [0,2) [2,5), y [0,1) [1,3)
        expect(t, "pdfimg box uneven", pishow(a, &s, 2, 2), "000000 000000 ffffff 555555");
        s = PIS("\x00\x00", 2, 4, 8, PDFCS_GRAY);  // three white rows past the data
        expect(t, "pdfimg box short", pishow(a, &s, 1, 2), "808080 ffffff");
        s = PIS("\x00\x00", 2, 4, 8, PDFCS_GRAY);
        expect(t, "pdfimg box short 1", pishow(a, &s, 1, 1), "bfbfbf");
    }

    // Memory follows the output: scan-ocr's 2550 by 3300 to 100 by 129
    // in less than the source would take as one row of RGBA pixels'
    // worth of rows, and work budgets and small arenas fail cleanly
    {
        Arena s   = *a;
        iz    n   = (iz)2550*3300;
        u8   *src = allocbytes(&s, n);
        for (iz i = 0; i < n; i++) {
            src[i] = (u8)(i%2550 < 1275 ? 0 : 255);
        }
        PdfSamples m = pisamples((char *)src, n, 2550, 3300, 8, PDFCS_GRAY);
        Arena out    = carve(&s, 100*129*4 + 2550*4 + 100*40 + 2*PDFIMG_CONVMEM + 512);
        i64   work   = I64MAX;
        Image p      = pdfpixels(&out, &m, 100, 129, &work);
        expect(t, "pdfimg big", p.err.data ? p.err : pxhex(&s, p.rgba, 8), "000000ff000000ff");
        expect(t, "pdfimg big right", p.err.data ? p.err : pxhex(&s, p.rgba+99*4, 4), "ffffffff");
        expect(t, "pdfimg big work", istr(&s, I64MAX-work < 3300*41 + 100), "1");

        Arena tiny = carve(&s, 100*129*4);
        byte *beg  = tiny.beg;
        p = pdfpixels(&tiny, &m, 100, 129, &work);
        expect(t, "pdfimg big oom", concat(&s, p.err, tiny.beg==beg ? Str("") : Str("!")),
               "out of memory to decode the image");
        work = 1000;
        beg  = s.beg;
        p = pdfpixels(&s, &m, 100, 129, &work);
        expect(t, "pdfimg big too much work", concat(&s, p.err, s.beg==beg ? Str("") : Str("!")),
               "too much work to decode the image");

        // No data at the largest size: white, quickly
        m.data = {};
        m.w    = PDFIMG_EDGE;
        m.h    = PDFIMG_EDGE;
        work   = I64MAX;
        p = pdfpixels(&s, &m, 64, 64, &work);
        expect(t, "pdfimg huge empty", p.err.data ? p.err : pxhex(&s, p.rgba, 4), "ffffffff");
        expect(t, "pdfimg huge empty work", istr(&s, I64MAX-work), "65536");
    }

    // pngfit: as it was in topng, now with sizes and soft failure
    {
        Arena s  = *a;
        u8   *px = fxpixels(&s, 37, 29, 0);
        Converted c = pngfit(&s, carve(&s, 8<<20), px, 37, 29, 8000, (i64)1<<26, 5<<20);
        expect(t, "pngfit same", istr(&s, imgdiff(s, c.png, px, 37, 29)), "0");
        c = pngfit(&s, carve(&s, 8<<20), px, 37, 29, 20, (i64)1<<26, 5<<20);
        expect(t, "pngfit edge", concat(&s, istr(&s, c.w), concat(&s, "x", istr(&s, c.h))), "20x15");
        c = pngfit(&s, carve(&s, 8<<20), px, 37, 29, 8000, 100, 5<<20);
        expect(t, "pngfit pixels", istr(&s, c.w*c.h<=100 && c.w>=8), "1");
        c = pngfit(&s, carve(&s, 8<<20), px, 37, 29, 8000, (i64)1<<26, 100);
        expect(t, "pngfit bytes", c.err, "the image is too large, and does not shrink to fit");
        byte *beg = s.beg;
        Arena mem = carve(&s, 4096);
        beg = s.beg;
        c = pngfit(&s, mem, px, 37, 29, 8000, (i64)1<<26, 5<<20);
        expect(t, "pngfit oom", concat(&s, c.err, s.beg==beg ? Str("") : Str("!")),
               "out of memory to convert the image");

        // No pixels, or so many that the cost would overflow: refused,
        // not an assert in shrink.
        c = pngfit(&s, carve(&s, 1<<20), px, 4, 0, 100, 100, 1<<20);
        expect(t, "pngfit no rows", c.err, "the image has no pixels, or too many");
        c = pngfit(&s, carve(&s, 1<<20), px, 0x7fffffff, 0x7fffffff, 100, 100, 1<<20);
        expect(t, "pngfit huge", c.err, "the image has no pixels, or too many");
    }

    // pngfit over scratch of every size: out of memory, or exactly the
    // full result, never a smaller image passed off as the answer.
    {
        Arena     s    = *a;
        u8       *px   = fxpixels(&s, 200, 150, 0);
        Converted full = pngfit(&s, carve(&s, 8<<20), px, 200, 150, 100, 1<<20, 5<<20);
        i32       bad  = 0;
        i32       oom  = 0;
        i32       runs = 0;
        for (iz n = 256<<10; n < 400<<10; n += 1021) {
            runs++;
            Arena     m = s;
            Converted c = pngfit(&m, carve(&m, n), px, 200, 150, 100, 1<<20, 5<<20);
            if (c.err == Str("out of memory to convert the image")) {
                oom++;
            } else if (c.err.data || c.w!=full.w || c.h!=full.h || !(c.png==full.png)) {
                bad++;
            }
        }
        expect(t, "pngfit sweep size", concat(&s, istr(&s, full.w), concat(&s, "x", istr(&s, full.h))), "100x75");
        expect(t, "pngfit sweep", istr(&s, bad), "0");
        expect(t, "pngfit sweep oom", istr(&s, oom>0 && oom<runs), "1");
    }

    // JPEG: passed through, or decoded and converted
    {
        Arena      s = *a;
        PdfSamples j = PIS(fxbaseline, 16, 16, 8, PDFCS_RGB);
        PdfShown   r = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8000, 1<<20, 5<<20);
        expect(t, "jpeg passed", concat(&s, r.mime, istr(&s, r.data==j.data)), "image/jpeg1");
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8, 1<<20, 5<<20);
        expect(t, "jpeg shrunk", concat(&s, r.mime, concat(&s, " ", istr(&s, r.w))), "image/png 8");
        j.w = 17;
        r   = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8000, 1<<20, 5<<20);
        expect(t, "jpeg size disagrees", concat(&s, r.mime, concat(&s, " ", istr(&s, r.w))),
               "image/png 16");
        j.w = 16;
        j.cs = PDFCS_GRAY;
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8000, 1<<20, 5<<20);
        expect(t, "jpeg space disagrees", r.mime, "image/png");
        j.cs = PDFCS_RGB;
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8000, 1<<20, base64len(j.data.len)-1);
        expect(t, "jpeg too big as base64", r.mime, "image/png");
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8000, 1<<20, 100);
        expect(t, "jpeg too big even shrunk", r.err, "the image is too large, and does not shrink to fit");

        // An inverted Decode inverts the pixels
        u8  *want = fxsmooth(&s, 16, 16);
        for (iz i = 0; i < 16*16; i++) {
            for (iz c = 0; c < 3; c++) {
                want[4*i+c] = (u8)(255 - want[4*i+c]);
            }
        }
        j.ndecode = 6;
        f32 inv[] = {1, 0, 1, 0, 1, 0};
        copybytes(j.decode, inv, sizeof(inv));
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &j, 8000, 1<<20, 5<<20);
        i32 d = imgdiff(s, r.data, want, 16, 16);
        expect(t, "jpeg decode inverted", istr(&s, d>=0 && d<=6), "1");

        // Ghostscript's CMYK, Adobe APP14, with Decode [1 0 1 0 1 0 1 0]:
        // cyan then black, as Poppler shows image-dct-cmyk too, the
        // Decode not applied over Wuffs' own inversion
        PdfSamples c = {};
        c.data    = pihex(&s, cmykjpeg);
        c.w       = 16;
        c.h       = 8;
        c.bpc     = 8;
        c.cs      = PDFCS_CMYK;
        c.ndecode = 8;
        f32 inv4[] = {1, 0, 1, 0, 1, 0, 1, 0};
        copybytes(c.decode, inv4, sizeof(inv4));
        expect(t, "jpeg cmyk adobe", istr(&s, pdfjpeg(c.data).adobe), "1");
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &c, 8000, 1<<20, 5<<20);
        Image p = imgdecode(&s, r.data, 1<<20);
        expect(t, "jpeg cmyk", p.rgba ? pxhex(&s, p.rgba, 4) : p.err, "00ffffff");
        expect(t, "jpeg cmyk black", p.rgba ? pxhex(&s, p.rgba+12*4, 4) : p.err, "000000ff");

        // A soft mask over a decoded JPEG
        PdfSamples m = PIS("\x00", 1, 1, 8, PDFCS_GRAY);
        c.smask = &m;
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &c, 8000, 1<<20, 5<<20);
        p = imgdecode(&s, r.data, 1<<20);
        expect(t, "jpeg smask", p.rgba ? pxhex(&s, p.rgba, 4) : p.err, "ffffffff");

        // Cut short, its header whole: never passed through, as an API
        // would refuse the request carrying it
        PdfSamples half = PIS(fxbaseline, 16, 16, 8, PDFCS_RGB);
        half.data.len /= 2;
        r = pdfshowjpeg(&s, carve(&s, 8<<20), &half, 8000, 1<<20, 5<<20);
        expect(t, "jpeg cut", istr(&s, r.mime!="image/jpeg"), "1");

        PdfSamples bad = PIS("\xff\xd8\xff\xc0\x00\x11\x08", 16, 16, 8, PDFCS_RGB);
        byte *beg = s.beg;
        r = pdfshowjpeg(&s, carve(&s, 1<<20), &bad, 8000, 1<<20, 5<<20);
        expect(t, "jpeg bad", istr(&s, r.err.len>0 && !r.data.data), "1");
        (void)beg;
    }
}
