use strict;
# Facts about overlay tablespace files, for the worktree overlay tests.
# Needs WT_DD (the datadir).
sub page0 { my $f = shift; open(my $h, '<:raw', $f) or die "$f: $!"; my $p; sysread($h, $p, 16384); return $p; }
sub page_n { my ($f, $n) = @_; open(my $h, '<:raw', $f) or die "$f: $!"; sysseek($h, $n * 16384, 0); my $p = ''; sysread($h, $p, 16384); return $p; }
sub page_is_zero { my $p = page_n(@_); return $p !~ /[^\0]/; }
sub overlay_bit { return (unpack('N', substr(page0(shift), 38 + 16, 4)) >> 15) & 1; }
sub link_of {
  my $p = page0(shift);
  return undef unless (unpack('N', substr($p, 38 + 16, 4)) >> 15) & 1;
  my $o = 16384 - 8 - 4096;
  return undef unless substr($p, $o, 4) eq 'WTM2';
  my ($na, $ni, $nr) = unpack('NNN', substr($p, $o + 4, 12));
  return substr($p, $o + 32 + 4 * $na + 16 * $ni + 4 * $nr, 32);
}
sub depth { my ($f, $d) = (shift, 0); while (my $u = link_of($f)) { $d++; $f = "$ENV{WT_DD}/#wtlayers/$u.ibl"; } return $d; }
sub nlink { return (stat(shift))[3]; }
sub layer_files { opendir(my $d, "$ENV{WT_DD}/#wtlayers") or return (); return grep { /\.ibl$/ } readdir($d); }
sub layers { my @l = layer_files(); return scalar @l; }
# Layers that no tablespace file's chain reaches (GC must have removed them).
sub stray {
  my %reach;
  for my $f (glob("$ENV{WT_DD}/*/*.ibd")) { my $x = $f; while (my $u = link_of($x)) { $reach{$u} = 1; $x = "$ENV{WT_DD}/#wtlayers/$u.ibl"; } }
  my @s = grep { !$reach{substr($_, 0, 32)} } layer_files(); return scalar @s;
}
sub wttmp { my @t = glob("$ENV{WT_DD}/*/*.wttmp"); return scalar @t; }
sub count_in_log { my ($f, $re) = @_; open(my $h, '<', $f) or die "$f: $!"; my $n = 0; while (<$h>) { $n++ if /$re/; } return $n; }
1;
