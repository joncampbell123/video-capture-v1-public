#!/usr/bin/perl
#
# Generate a SCC file that tests the charset of the EIA-608 caption decoder.
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

# Basic charset test ====================================================================
$msg = "Basic charset test >>";
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

for ($i=0x20;$i < 0x80;$i += 32) {
	print secondstotimecode($seconds)."   "; $seconds += 4;

	print "9420 94ae 9350 ";
	$msg = sprintf("[0x%02x]",$i);
	for ($j=0;$j < length($msg);$j += 2) {
		$cc1 = ord(substr($msg,$j,1));
		$cc2 = ord(substr($msg,$j+1,1));
		print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
	}

	print "9370 ";

	$msg = "....:....:....:....:....:....:..";
	for ($j=0;$j < length($msg);$j += 2) {
		$cc1 = ord(substr($msg,$j,1));
		$cc2 = ord(substr($msg,$j+1,1));
		print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
	}

	print "9450 ";
	for ($j=0;$j < 32;$j += 2) {
		print sprintf("%04x ",ccparity(($i+$j+1) + (($i+$j) << 8)));
	}
	print "142c 142f ";
	print "\n";
}

my @modes = qw(straight pad double doublepad);

# Special charset test ====================================================================
$msg = "Special charset test >>";
print secondstotimecode($seconds)."   "; $seconds += 2;
print "9420 94ae 9452 97a1 91a2 ";
for ($j=0;$j < length($msg);$j += 2) {
	$cc1 = ord(substr($msg,$j,1));
	$cc2 = ord(substr($msg,$j+1,1));
	print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
}
print "142c 142f ";
print "\n";

for ($mode=0;$mode < @modes;$mode++) {
	for ($i=0x30;$i < 0x40;$i += 16) {
		$mode_str = $modes[$mode];

		print secondstotimecode($seconds)."   "; $seconds += 4;

		print "9420 94ae 9350 ";
		$msg = sprintf("[0x%02x] %s",$i,$mode_str);
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9370 ";

		$msg = "....:....:....:.";
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9450 ";
		for ($j=0;$j < 16;$j++) {
			# NTS: weird conventional wisdom that nobody explains properly
			#      out there: special characters (like the note) are
			#      transmitted twice, or require padding after it.
			#      A test laserdisc of mine for instance, a Disney film
			#      while they sing, transmits music notes twice for each
			#      music note to appear. My TV set seems satisfied with
			#      one word of padding to properly display the characters.
			print sprintf("%04x ",ccparity($j + $i + (0x11 << 8)));

			if ($mode_str eq "straight") {
			}
			elsif ($mode_str eq "pad") {
				print "8080 ";
			}
			elsif ($mode_str eq "double") {
				print sprintf("%04x ",ccparity($j + $i + (0x11 << 8)));
			}
			elsif ($mode_str eq "doublepad") {
				print sprintf("%04x ",ccparity($j + $i + (0x11 << 8)));
				print "8080 ";
			}
		}
		print "142c 142f ";
		print "\n";
	}
}

my @modes = qw(charow charowpad straight pad double doublepad);

# Spanish/french charset test ====================================================================
$msg = "Spanish/french test >>";
print secondstotimecode($seconds)."   "; $seconds += 2;
print "9420 94ae 9452 97a1 91a2 ";
for ($j=0;$j < length($msg);$j += 2) {
	$cc1 = ord(substr($msg,$j,1));
	$cc2 = ord(substr($msg,$j+1,1));
	print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
}
print "142c 142f ";
print "\n";

for ($mode=0;$mode < @modes;$mode++) {
	# NTS: The LG television I was testing on apparently couldn't handle these
	#      extended overwrite chars out to the 31st column properly. So we show
	#      it in 16-char bundles.
	for ($i=0x20;$i < 0x40;$i += 0x10) {
		$mode_str = $modes[$mode];

		print secondstotimecode($seconds)."   "; $seconds += 3;

		print "9420 94ae 9350 ";
		$msg = sprintf("[0x%02x] %s",$i,$mode_str);
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9370 ";

		$msg = "....:....:....:.";
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9450 ";
		for ($j=0;$j < 0x10;$j++) {
			if ($mode_str eq "charow" || $mode_str eq "charowpad") { # char overwrite i.e. the extended char overwrites ASCII
				print sprintf("%04x ",ccparity(ord('.') << 8));
			}

			print sprintf("%04x ",ccparity($i + $j + (0x12 << 8)));

			if ($mode_str eq "straight") {
			}
			elsif ($mode_str eq "pad" || $mode_str eq "charowpad") {
				print "8080 ";
			}
			elsif ($mode_str eq "double") {
				print sprintf("%04x ",ccparity($i + $j + (0x12 << 8)));
			}
			elsif ($mode_str eq "doublepad") {
				print sprintf("%04x ",ccparity($i + $j + (0x12 << 8)));
				print "8080 ";
			}
		}
		# FIXME: You know how normally a TV decoder will type out your char, then
		#        fill in the next slot with non-transparent space? Apparently TVs
		#        don't do that for these special chars
		print "142c 142f ";
		print "\n";
	}
}

for ($mode=0;$mode < @modes;$mode++) {
	for ($i=0x20;$i < 0x40;$i += 0x20) {
		$mode_str = $modes[$mode];

		print secondstotimecode($seconds)."   "; $seconds += 3;

		print "9420 94ae 9350 ";
		$msg = sprintf("[0x%02x] %s x 32",$i,$mode_str);
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9370 ";

		$msg = "....:....:....:....:....:....:..";
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9450 ";
		for ($j=0;$j < 0x20;$j++) {
			if ($mode_str eq "charow" || $mode_str eq "charowpad") { # char overwrite i.e. the extended char overwrites ASCII
				print sprintf("%04x ",ccparity(ord('.') << 8));
			}

			print sprintf("%04x ",ccparity($i + $j + (0x12 << 8)));

			if ($mode_str eq "straight") {
			}
			elsif ($mode_str eq "pad" || $mode_str eq "charowpad") {
				print "8080 ";
			}
			elsif ($mode_str eq "double") {
				print sprintf("%04x ",ccparity($i + $j + (0x12 << 8)));
			}
			elsif ($mode_str eq "doublepad") {
				print sprintf("%04x ",ccparity($i + $j + (0x12 << 8)));
				print "8080 ";
			}
		}
		# FIXME: You know how normally a TV decoder will type out your char, then
		#        fill in the next slot with non-transparent space? Apparently TVs
		#        don't do that for these special chars
		print "142c 142f ";
		print "\n";
	}
}

# Portuguese/german/danish charset test ====================================================================
$msg = "Portuguese/german/danish >>";
print secondstotimecode($seconds)."   "; $seconds += 2;
print "9420 94ae 9452 97a1 91a2 ";
for ($j=0;$j < length($msg);$j += 2) {
	$cc1 = ord(substr($msg,$j,1));
	$cc2 = ord(substr($msg,$j+1,1));
	print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
}
print "142c 142f ";
print "\n";

for ($mode=0;$mode < @modes;$mode++) {
	# NTS: The LG television I was testing on apparently couldn't handle these
	#      extended overwrite chars out to the 31st column properly. So we show
	#      it in 16-char bundles.
	for ($i=0x20;$i < 0x40;$i += 0x10) {
		$mode_str = $modes[$mode];

		print secondstotimecode($seconds)."   "; $seconds += 3;

		print "9420 94ae 9350 ";
		$msg = sprintf("[0x%02x] %s",$i,$mode_str);
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9370 ";

		$msg = "....:....:....:.";
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9450 ";
		for ($j=0;$j < 0x10;$j++) {
			if ($mode_str eq "charow" || $mode_str eq "charowpad") { # char overwrite i.e. the extended char overwrites ASCII
				print sprintf("%04x ",ccparity(ord('.') << 8));
			}

			print sprintf("%04x ",ccparity($i + $j + (0x13 << 8)));

			if ($mode_str eq "straight") {
			}
			elsif ($mode_str eq "pad" || $mode_str eq "charowpad") {
				print "8080 ";
			}
			elsif ($mode_str eq "double") {
				print sprintf("%04x ",ccparity($i + $j + (0x13 << 8)));
			}
			elsif ($mode_str eq "doublepad") {
				print sprintf("%04x ",ccparity($i + $j + (0x13 << 8)));
				print "8080 ";
			}
		}
		# FIXME: You know how normally a TV decoder will type out your char, then
		#        fill in the next slot with non-transparent space? Apparently TVs
		#        don't do that for these special chars
		print "142c 142f ";
		print "\n";
	}
}

for ($mode=0;$mode < @modes;$mode++) {
	for ($i=0x20;$i < 0x40;$i += 0x20) {
		$mode_str = $modes[$mode];

		print secondstotimecode($seconds)."   "; $seconds += 3;

		print "9420 94ae 9350 ";
		$msg = sprintf("[0x%02x] %s x 32",$i,$mode_str);
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9370 ";

		$msg = "....:....:....:....:....:....:..";
		for ($j=0;$j < length($msg);$j += 2) {
			$cc1 = ord(substr($msg,$j,1));
			$cc2 = ord(substr($msg,$j+1,1));
			print sprintf("%04x ",ccparity($cc2 + ($cc1 << 8)));
		}

		print "9450 ";
		for ($j=0;$j < 0x20;$j++) {
			if ($mode_str eq "charow" || $mode_str eq "charowpad") { # char overwrite i.e. the extended char overwrites ASCII
				print sprintf("%04x ",ccparity(ord('.') << 8));
			}

			print sprintf("%04x ",ccparity($i + $j + (0x13 << 8)));

			if ($mode_str eq "straight") {
			}
			elsif ($mode_str eq "pad" || $mode_str eq "charowpad") {
				print "8080 ";
			}
			elsif ($mode_str eq "double") {
				print sprintf("%04x ",ccparity($i + $j + (0x13 << 8)));
			}
			elsif ($mode_str eq "doublepad") {
				print sprintf("%04x ",ccparity($i + $j + (0x13 << 8)));
				print "8080 ";
			}
		}
		# FIXME: You know how normally a TV decoder will type out your char, then
		#        fill in the next slot with non-transparent space? Apparently TVs
		#        don't do that for these special chars
		print "142c 142f ";
		print "\n";
	}
}

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


