#define pr_fmt(fmt) "EFI: "fmt
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/err.h>
#include <linux/efi.h>

int __init parse_efi_signature_list(
	const char *source,
	const void *data, size_t size,
	efi_element_handler_t (*get_handler_for_guid)(const efi_guid_t *))
{
	efi_element_handler_t handler;
	unsigned int offs = 0;

	pr_devel("-->%s(,%zu)\n", __func__, size);

	while (size > 0) {
		const efi_signature_data_t *elem;
		efi_signature_list_t list;
		size_t lsize, esize, hsize, elsize;

		if (size < sizeof(list)) {
			pr_err("Invalid size %zu for signature list\n", size);
			return -EINVAL;
		}

		memcpy(&list, data, sizeof(list));
		pr_devel("LIST[%04x] guid=%pUl ls=%x hs=%x ss=%x\n",
			 offs,
			 &list.signature_type, list.signature_list_size,
			 list.signature_header_size, list.signature_size);

		lsize = list.signature_list_size;
		hsize = list.signature_header_size;
		esize = list.signature_size;
		elsize = lsize - sizeof(list) - hsize;

		if (lsize > size) {
			pr_err("Invalid size %zu for signature list\n", size);
			pr_devel("<--%s() = -EBADMSG [overrun @%x]\n",
				 __func__, offs);
			return -EBADMSG;
		}

		if (lsize < sizeof(list) ||
		    lsize - sizeof(list) < hsize ||
		    esize < sizeof(*elem) ||
		    elsize < esize ||
		    elsize % esize != 0) {
			pr_err("Invalid size combo @%x\n", offs);
			return -EBADMSG;
		}

		handler = get_handler_for_guid(&list.signature_type);
		if (!handler) {
			data += lsize;
			size -= lsize;
			offs += lsize;
			continue;
		}

		data += sizeof(list) + hsize;
		size -= sizeof(list) + hsize;
		offs += sizeof(list) + hsize;

		for (; elsize > 0; elsize -= esize) {
			elem = data;

			pr_devel("ELEM[%04x]\n", offs);
			if (handler(source,
				&elem->signature_data,
				esize - sizeof(*elem)) < 0) {
				pr_err("Error handling signature element\n");
				return -EINVAL;
			}

			data += esize;
			size -= esize;
			offs += esize;
		}
	}

	return 0;
}
