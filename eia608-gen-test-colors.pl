#!/usr/bin/perl
#
# Generate a SCC file that tests the color modes of the EIA-608 caption decoder.
# You are expected to redirect our STDOUT to a file.

sub ccparitybyte($) {
	my $i,$p=1;

	for ($i=0;$i < 8;$i++) {
		if ($_[0] & (1 << $i)) {
			$p ^= 1;
		}
	}

	return 0x80 if $p != 0;
	return 0x00;
}

sub ccparity($) {
	my $t = $_[0];

	$t &= 0x7F7F;
	$t |= ccparitybyte($t >> 8) << 8;
	$t |= ccparitybyte($t & 0xFF);
	return $t;
}

sub secondstotimecode($) {
	return sprintf("%02u:%02u:%02u:00",$_[0] / 3600,($_[0] / 60) % 60,$_[0] % 60);
}

my $seconds = 2;

print "Scenarist_SCC V1.0\n";
print "\n";

# Foreground color test ====================================================================
$msg = "Foreground colors >>";
print secondstotimecode($seconds)."   "; $seconds += 2;
print "1724 "; # switch to standard (norpak)
print "9420 94ae 9452 97a1 91a2 ";
for ($j=0;$j < length($msg);$j += 2) {
	$cc1 = ord(substr($msg,$j,1));
	$cc2 = ord(substr($msg,$j+1,1));
	print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
}
print "142c 142f ";
print "\n";

print secondstotimecode($seconds)."   "; $seconds += 5;

print "9420 94ae ";

print "9370 ";

# NTS: Color attribute changes are "spacing" meaning they create a space
$msg = "....:....:....:.";
for ($j=0;$j < length($msg);$j++) {
	$cc1 = ord(substr($msg,$j,1));
	print sprintf("%04x ",ccparity($cc1 + (0x20 << 8)));
}

print "9450 ";
for ($j=0;$j < 16;$j++) {
	print sprintf("%04x ",ccparity(0x1120 + $j));
	print sprintf("%04x ",ccparity(ord('x') << 8));
}
print "142c 142f ";
print "\n";

# Background color test ====================================================================
$msg = "Background colors >>";
print secondstotimecode($seconds)."   "; $seconds += 2;
print "1724 "; # switch to standard (norpak)
print "9420 94ae 9452 97a1 91a2 ";
for ($j=0;$j < length($msg);$j += 2) {
	$cc1 = ord(substr($msg,$j,1));
	$cc2 = ord(substr($msg,$j+1,1));
	print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
}
print "142c 142f ";
print "\n";

print secondstotimecode($seconds)."   "; $seconds += 5;

print "9420 94ae ";

print "9370 ";

# NTS: Background color attribute changes are "non-spacing", they do not cause an extra space
$msg = "....:....:....:.";
for ($j=0;$j < length($msg);$j++) {
	$cc1 = ord(substr($msg,$j,1));
	print sprintf("%04x ",ccparity($cc1 + (0x20 << 8)));
}

print "9450 ";
for ($j=0;$j < 16;$j++) {
	print sprintf("%04x ",ccparity(0x1020 + $j));
	print sprintf("%04x ",ccparity(ord('x') + (0x20 << 8)));
}
print "142c 142f ";
print "\n";

# goodbye message =======================================================================
$msg = ">> End of test";
print secondstotimecode($seconds)."   "; $seconds += 4;
print "9420 94ae 9452 97a1 91a0 ";
for ($j=0;$j < length($msg);$j += 2) {
	$cc1 = ord(substr($msg,$j,1));
	$cc2 = ord(substr($msg,$j+1,1));
	print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
}
print "142c 142f ";
print "\n";

# goodbye message
print secondstotimecode($seconds)."   "; $seconds += 4;
print "9420 94ae 9452 97a1 91a0 ";
print "142c 142f ";
print "\n";


