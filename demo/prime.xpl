\PRIME.XPL
\Eratosthenes Sieve Prime Number Program in XPL0

code RESERVE=3, CRLF=9, INTOUT=11, TEXT=12, REM=2, CHOUT=8,NUMIN=10;

define SIZE=8190;

character FLAGS;
integer I,PRIME,K,COUNT,ITER,DEV;

begin
FLAGS:=RESERVE(SIZE+1);
COUNT:=0;
TEXT(0,"What input device. 0=crt 2=printer ");
DEV:=NUMIN(0);
for I:=0,SIZE do FLAGS(I):=true;
for I:=0,SIZE do
	begin
	if FLAGS(I) then
		begin
		PRIME:=I+I+3;
		K:=I+PRIME;
		while K<=SIZE do
			begin
			FLAGS(K):=false;
			K:=K+PRIME;
			end;
		COUNT:=COUNT+1;
		INTOUT(DEV,PRIME); TEXT(DEV,"	");
		if REM((COUNT/9))=8 then CRLF(DEV);
		end;
	end;
CRLF(DEV);
INTOUT(DEV,COUNT); TEXT(DEV," primes"); CRLF(DEV);
end;
LPRIME   BAKPRIME   I2LŒÀ`ˆ¥è‘ ”ˆ–è‘ ”ˆ–‘‘È”Ð–‘‘È”Ð–‘£È ¥…¤

  1