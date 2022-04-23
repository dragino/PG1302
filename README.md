# dragino_fwd_for_pi dragion gateway forward for pi

2021/11/22  
add test_fcc_complian.c
fcc pre_compilance test app

2021/12/03
edit fwd/src/pkt_serv.c
adjust dn_list, remove pacakage from dn_list when have the same devaddr, or if the dn_list size more than 16

2021.12.16
modify fwd/src/pkt_serv.c
remove element from dn_list when size biger than 16
remove element from dn_list when devaddr duplicated
change the loop time of read_dir
change fn search_dn_list to inline fn

2022.04.20
update station 
fix make deb to -Zgzip
