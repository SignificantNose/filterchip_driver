savedcmd_/home/significantnose/tryingfunstuff/drivers/coursework/filterchip.mod := printf '%s\n'   fchip_codec.o fchip_posfix.o fchip_vga.o fchip_hda_bus.o fchip_int.o fchip_pcm.o fchip.o | awk '!x[$$0]++ { print("/home/significantnose/tryingfunstuff/drivers/coursework/"$$0) }' > /home/significantnose/tryingfunstuff/drivers/coursework/filterchip.mod
